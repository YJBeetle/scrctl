# frozen_string_literal: true

# Run with HOMEBREW_DEVELOPER=1 HOMEBREW_NO_AUTO_UPDATE=1 brew ruby this-file
# INSTALL_DIRECTORY OUTPUT_DIRECTORY. Loads the recipe retained in each installed
# keg, never the current formula from the API/tap. No package is installed.
require "json"
require "digest"
require "fileutils"
require "pathname"
require "tmpdir"
require "open3"

module MacosSources
  module_function

  def digest(path)
    Digest::SHA256.file(path).hexdigest
  end

  def command(*args)
    stdout, stderr, status = Open3.capture3(*args)
    raise "Command failed: #{args.first}: #{stderr}" unless status.success?

    stdout.strip
  end

  def token(value)
    raise "Invalid package/version component: #{value.inspect}" unless value.is_a?(String) && value.match?(/\A[A-Za-z0-9][A-Za-z0-9@+_.-]*\z/)

    value
  end

  def file_record(path, root)
    { "path" => path.relative_path_from(root).to_s,
      "sha256" => digest(path), "size" => path.size }
  end

  def fetch_resource(resource)
    attempts = 0
    begin
      attempts += 1
      resource.fetch(timeout: 300, skip_patches: true, quiet: true)
    rescue StandardError => error
      # curl's built-in --retry does not cover partial transfers (exit 18).
      # Homebrew resumes its own incomplete download on the next fetch.
      raise unless defined?(DownloadError) && error.is_a?(DownloadError) && attempts < 3

      warn "Retrying incomplete Homebrew source download (#{attempts}/2): #{resource.url}"
      retry
    end
  end

  # Homebrew warns rather than fails when a checksum is missing. Require an
  # immutable digest ourselves before calling its downloader, and verify again.
  def export_resource(resource, directory, root, label)
    expected = resource.checksum&.hexdigest
    revision = resource.specs[:revision]
    immutable_git = expected.nil? && revision.is_a?(String) && revision.match?(/\A[0-9a-f]{40}\z/i)
    raise "Unpinned source: #{label}: #{resource.url}" unless expected&.match?(/\A[0-9a-f]{64}\z/i) || immutable_git

    cached = Pathname(fetch_resource(resource))
    directory.mkpath
    if cached.file?
      raise "Missing checksum for archive: #{label}" unless expected
      raise "Source checksum mismatch: #{label}" unless digest(cached) == expected.downcase

      destination = directory/cached.basename
      FileUtils.cp(cached, destination)
      record = file_record(destination, root)
    elsif cached.directory? && immutable_git
      actual = command("git", "-C", cached.to_s, "rev-parse", "HEAD")
      raise "VCS revision mismatch: #{label}: #{actual}" unless actual.casecmp?(revision)
      # Include .git: some build recipes derive the version from git describe.
      # Homebrew also fetches recursive submodules. Their exact checkout and
      # metadata are retained, rather than losing them with git archive.
      status = command("git", "-C", cached.to_s, "status", "--porcelain", "--untracked-files=all")
      raise "Dirty VCS source: #{label}" unless status.empty?
      submodules = command("git", "-C", cached.to_s, "submodule", "status", "--recursive")
      raise "Unresolved VCS submodule: #{label}" if submodules.lines.any? { |line| line.start_with?("-", "+", "U") }
      destination = directory/"source-#{revision}.tar.gz"
      command("tar", "-czf", destination.to_s, "-C", cached.to_s, ".")
      record = file_record(destination, root).merge("git_revision" => actual, "git_submodules" => submodules)
    else
      raise "Unsupported source download: #{label}: #{cached}"
    end
    cache_relative = if defined?(HOMEBREW_CACHE)
      cached.expand_path.relative_path_from(HOMEBREW_CACHE.expand_path).to_s
    end
    raise "Download outside Homebrew cache: #{cached}" if cache_relative && (cache_relative == ".." || cache_relative.start_with?("../"))
    record.merge("role" => label, "url" => resource.url,
                 "declared_sha256" => expected, "download_strategy" => resource.downloader.class.name,
                 "homebrew_cache_path" => cache_relative,
                 "specs" => resource.specs.transform_keys(&:to_s))
  end

  def export_patch(patch, directory, root, recipe)
    if patch.external?
      export_resource(patch.resource, directory, root, "patch").merge(
        "strip" => patch.strip.to_s, "apply" => patch.patch_files.map(&:to_s))
    else
      patch.path = recipe if defined?(DATAPatch) && patch.is_a?(DATAPatch)
      directory.mkpath
      destination = directory/"embedded.patch"
      destination.binwrite(patch.contents)
      file_record(destination, root).merge("role" => "patch", "strip" => patch.strip.to_s,
                                          "directory" => patch.directory&.to_s)
    end
  end

  def collect_package(name, info, dependencies, install, root, loader)
    version = token(info.fetch("version"))
    candidates = dependencies.select { |entry| entry["homebrew_package"] == "#{name}/#{version}" }
    raise "Package has no runtime libraries: #{name}" if candidates.empty?
    kegs = candidates.map do |entry|
      original = Pathname(entry.fetch("original_path")).realpath
      keg = original.ascend.find { |parent| (parent/"INSTALL_RECEIPT.json").file? }
      raise "Missing keg receipt: #{original}" unless keg
      raise "Wrong keg version: #{keg}" unless keg.basename.to_s == version && keg.parent.basename.to_s == name
      raise "Original runtime library changed: #{original}" unless digest(original) == entry.fetch("original_sha256")

      keg
    end.uniq
    raise "Multiple kegs for #{name}/#{version}" unless kegs.length == 1
    keg = kegs.first
    receipt = JSON.parse((keg/"INSTALL_RECEIPT.json").read)
    raise "Only stable Homebrew recipes supported: #{name}" unless receipt.dig("source", "spec") == "stable"
    raise "Receipt source changed: #{name}" unless receipt["source"] == info.fetch("source")
    recipes = (keg/".brew").glob("*.rb")
    recipe = keg/".brew"/"#{name}.rb"
    raise "Missing exact keg formula: #{recipe}" unless recipe.file? && recipes.length == 1
    preserved = install/"share/doc/scrctl/dependency-licenses"/name/"formula"/recipe.basename
    raise "Recorded formula differs from installed recipe: #{name}" unless preserved.file? && digest(preserved) == digest(recipe)
    formula = loader.call(recipe)
    raise "Recipe version mismatch: #{name}: #{formula.pkg_version}" unless formula.pkg_version.to_s == version
    raise "Missing stable recipe: #{name}" unless formula.stable
    # A fetch hook can download undeclared inputs. Reject until those inputs are
    # represented explicitly; otherwise a successful export would be misleading.
    raise "Recipe fetch hook needs explicit source handling: #{name}" if formula.fetch_defined?

    puts "Collecting exact Homebrew sources: #{name} #{version}"

    directory = root/"packages"/name/version
    directory.mkpath
    FileUtils.cp(keg/"INSTALL_RECEIPT.json", directory)
    FileUtils.cp_r(keg/".brew", directory/"recipe")
    artifacts = [file_record(directory/"INSTALL_RECEIPT.json", root),
                 file_record(directory/"recipe"/recipe.basename, root)]
    artifacts << export_resource(formula.stable, directory/"downloads/main", root, "source")
    formula.resources.each_with_index do |resource, index|
      artifacts << export_resource(resource, directory/"downloads"/"resource-#{index}", root, "resource:#{resource.name}")
      resource.patches.each_with_index do |patch, patch_index|
        artifacts << export_patch(patch, directory/"patches"/"resource-#{index}-#{patch_index}", root, recipe)
      end
    end
    formula.patchlist.each_with_index do |patch, index|
      artifacts << export_patch(patch, directory/"patches"/"main-#{index}", root, recipe)
    end
    manifest = { "name" => name, "version" => version, "recipe_sha256" => digest(recipe),
                 "receipt_source" => receipt["source"], "built_on" => receipt["built_on"],
                 "homebrew_version" => receipt["homebrew_version"],
                 "dependencies" => formula.deps.map { |dep| { "name" => dep.name, "tags" => dep.tags.map(&:to_s) } },
                 "artifacts" => artifacts }
    (directory/"manifest.json").write(JSON.pretty_generate(manifest) + "\n")
    manifest
  end

  def collect(install, output, loader:, brew_info:)
    install = Pathname(install).realpath
    output = Pathname(output).expand_path
    raise "Source output already exists: #{output}" if output.exist?
    metadata = JSON.parse((install/"share/doc/scrctl/package-metadata.json").read)
    dependencies = metadata.fetch("runtime_dependencies")
    packages = metadata.fetch("homebrew_packages")
    dependencies.each do |entry|
      owner = entry["homebrew_package"]
      embedded = entry["source_dependency"] == "xdb" && entry["library"].match?(/\Alibxdb_client(?:\.\d+)*\.dylib\z/)
      raise "Unowned bundled runtime library: #{entry['library']}" unless embedded || (owner && packages.any? { |name, data| "#{name}/#{data['version']}" == owner })
      bundled = install/"bin/lib"/entry.fetch("library")
      raise "Bundled runtime library changed: #{bundled}" unless digest(bundled) == entry.fetch("bundled_sha256")
    end
    output.parent.mkpath
    temporary = Dir.mktmpdir(".sources-", output.parent.to_s)
    begin
      root = Pathname(temporary)
      results = packages.sort.map do |name, info|
        collect_package(token(name), info, dependencies, install, root, loader)
      end
      manifest = { "schema_version" => 1, "platform" => "macos", "package_manager" => "homebrew",
                   "source_commit" => metadata.fetch("source_commit"),
                   "package_metadata_sha256" => digest(install/"share/doc/scrctl/package-metadata.json"),
                   "collector_homebrew" => brew_info,
                   "packages" => results }
      FileUtils.cp(install/"share/doc/scrctl/package-metadata.json", root/"package-metadata.json")
      (root/"source-manifest.json").write(JSON.pretty_generate(manifest) + "\n")
      (root/"README.txt").write("Exact installed Homebrew recipes, upstream sources, resources and patches.\n" \
        "Recipes contain install/configure and inreplace changes; INSTALL_RECEIPT.json records the original bottle build environment.\n" \
        "Archive downloads match the recipe SHA-256. VCS snapshots retain pinned commits, submodules and .git metadata.\n" \
        "REBUILD: use a separate Homebrew checkout at collector_homebrew.git_commit from https://github.com/Homebrew/brew.\n" \
        "Original bottle builders may have used a different Homebrew version; each receipt preserves that version and built_on.\n" \
        "Create a local/scrctl tap with brew tap-new local/scrctl and copy packages/*/*/recipe/*.rb to its Formula directory.\n" \
        "Install the manifest's declared build dependencies first; rebuild the saved runtime recipes in dependency order\n" \
        "with HOMEBREW_NO_AUTO_UPDATE=1 brew install --build-from-source local/scrctl/NAME.\n" \
        "CACHE REPLAY: verify each artifact SHA-256, then copy archives to $(brew --cache)/homebrew_cache_path\n" \
        "from their manifest entry. For Git snapshots, create that cache directory and extract the snapshot into it instead.\n" \
        "Embedded patches remain in the saved recipe; external patch archives have their own cache paths.\n" \
        "Homebrew still validates archive checksums and checkout revisions. Cache replay avoids source re-downloads.\n" \
        "These inputs permit rebuilding but do not promise byte-identical bottles; toolchain/macOS versions affect output.\n" \
        "This contains runtime dependency sources, not macOS system frameworks or all compiler/build-tool sources.\n")
      FileUtils.mv(root, output)
    ensure
      FileUtils.rm_rf(temporary)
    end
  end
end

if $PROGRAM_NAME == __FILE__
  abort "Usage: HOMEBREW_DEVELOPER=1 brew ruby #{$PROGRAM_NAME} INSTALL_DIRECTORY OUTPUT_DIRECTORY" unless ARGV.length == 2
  require "formula"
  require "formulary"
  MacosSources.collect(ARGV[0], ARGV[1],
    loader: ->(path) { Formulary.factory(path, :stable) },
    brew_info: { "version" => HOMEBREW_VERSION,
                 "git_commit" => MacosSources.command("git", "-C", HOMEBREW_REPOSITORY.to_s, "rev-parse", "HEAD") })
end
