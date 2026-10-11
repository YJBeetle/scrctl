# frozen_string_literal: true
require "minitest/autorun"
require_relative "../tools/collect_macos_sources"
DownloadError = Class.new(StandardError) unless defined?(DownloadError)

class MacosSourcesTest < Minitest::Test
  Checksum = Struct.new(:hexdigest)
  Dependency = Struct.new(:name, :tags)
  Patch = Struct.new(:contents, :strip, :directory) do
    def external?; false; end
  end
  External = Struct.new(:resource, :strip, :patch_files) do
    def external?; true; end
  end
  Formula = Struct.new(:pkg_version, :stable, :resources, :patchlist, :deps, :fetch_hook) do
    def fetch_defined?; fetch_hook; end
  end
  Resource = Struct.new(:path, :checksum, :name, :patches, :specs) do
    def url; "https://example.invalid/#{name}.tar.gz"; end
    def fetch(**_options); path; end
    def downloader; self; end
  end

  def setup
    @temp = Pathname(Dir.mktmpdir("macos-sources-test-"))
    @install = @temp/"install"
    @output = @temp/"sources"
    @doc = @install/"share/doc/scrctl"
    @doc.mkpath
    @keg = @temp/"Cellar/example/1.2_1"
    (@keg/".brew").mkpath
    (@keg/"lib").mkpath
    @original = @keg/"lib/libexample.dylib"
    @original.write("original binary")
    @recipe = @keg/".brew/example.rb"
    @recipe.write("class Example < Formula; end\n")
    @source = { "spec" => "stable", "tap" => "homebrew/core" }
    (@keg/"INSTALL_RECEIPT.json").write(JSON.generate("source" => @source, "homebrew_version" => "test"))
    preserved = @doc/"dependency-licenses/example/formula"
    preserved.mkpath
    FileUtils.cp(@recipe, preserved)
    bundled = @install/"bin/lib/libexample.dylib"
    bundled.parent.mkpath
    bundled.write("relocated binary")
    @metadata = {
      "source_commit" => "a" * 40,
      "homebrew_packages" => { "example" => { "version" => "1.2_1", "source" => @source } },
      "runtime_dependencies" => [{ "library" => "libexample.dylib",
        "original_path" => @original.to_s, "original_sha256" => MacosSources.digest(@original),
        "bundled_sha256" => MacosSources.digest(bundled), "homebrew_package" => "example/1.2_1" }]
    }
    @formula = Formula.new("1.2_1", resource("source"), [], [], [Dependency.new("cmake", [:build])], false)
  end

  def teardown
    FileUtils.rm_rf(@temp)
  end

  def resource(name)
    path = @temp/"#{name}.tar.gz"
    path.write("archive #{name}")
    Resource.new(path, Checksum.new(MacosSources.digest(path)), name, [], {})
  end

  def collect
    (@doc/"package-metadata.json").write(JSON.generate(@metadata))
    MacosSources.collect(@install, @output, loader: ->(_path) { @formula }, brew_info: { "git_commit" => "b" * 40 })
  end

  def assert_failed(message)
    error = assert_raises(RuntimeError) { collect }
    assert_match(message, error.message)
    refute @output.exist?, "Failed export must not publish an incomplete source tree"
    assert_empty @temp.glob(".sources-*")
  end

  def test_exports_exact_recipe_resources_and_both_patch_kinds
    extra = resource("resource")
    extra.patches << Patch.new("resource patch", :p1, "subdir")
    @formula.resources << extra
    @formula.patchlist << External.new(resource("external-patch"), :p0, ["fix.patch"])
    @formula.patchlist << Patch.new("inline patch", :p1, nil)
    collect
    manifest = JSON.parse((@output/"source-manifest.json").read)
    package = manifest.fetch("packages").first
    assert_equal "1.2_1", package.fetch("version")
    assert_equal MacosSources.digest(@recipe), package.fetch("recipe_sha256")
    assert_equal [{ "name" => "cmake", "tags" => ["build"] }], package.fetch("dependencies")
    assert_equal 7, package.fetch("artifacts").length
    package.fetch("artifacts").each do |entry|
      assert_equal entry.fetch("sha256"), MacosSources.digest(@output/entry.fetch("path"))
    end
    assert_equal 3, package.fetch("artifacts").count { |entry| entry["role"] == "patch" }
  end

  def test_rejects_changed_bundled_library
    (@install/"bin/lib/libexample.dylib").write("changed")
    assert_failed(/Bundled runtime library changed/)
  end

  def test_rejects_unknown_owner
    @metadata["runtime_dependencies"].first["homebrew_package"] = nil
    assert_failed(/Unowned/)
  end

  def test_rejects_changed_original_library
    @original.write("changed")
    assert_failed(/Original runtime library changed/)
  end

  def test_rejects_recipe_changed_since_provenance_capture
    @recipe.write("different recipe")
    assert_failed(/Recorded formula differs/)
  end

  def test_rejects_wrong_loaded_version
    @formula.pkg_version = "2.0"
    assert_failed(/Recipe version mismatch/)
  end

  def test_rejects_missing_checksum
    @formula.stable.checksum = nil
    assert_failed(/Unpinned source/)
  end

  def test_rejects_checksum_mismatch_even_if_native_fetch_returns_it
    @formula.stable.path.write("tampered archive")
    assert_failed(/Source checksum mismatch/)
  end

  def test_rejects_undeclared_fetch_hook
    @formula.fetch_hook = true
    assert_failed(/fetch hook/)
  end

  def test_rejects_mutable_vcs_branch
    @formula.stable.checksum = nil
    @formula.stable.specs = { branch: "master" }
    assert_failed(/Unpinned source/)
  end

  def vcs_resource
    checkout = @temp/"checkout"
    checkout.mkpath
    MacosSources.command("git", "-C", checkout.to_s, "init", "-q")
    (checkout/"source.c").write("int example;\n")
    MacosSources.command("git", "-C", checkout.to_s, "add", "source.c")
    MacosSources.command("git", "-C", checkout.to_s, "-c", "user.name=Test", "-c",
                         "user.email=test@example.invalid", "commit", "-qm", "source")
    revision = MacosSources.command("git", "-C", checkout.to_s, "rev-parse", "HEAD")
    Resource.new(checkout, nil, "vcs", [], { revision: revision })
  end

  def test_retains_immutable_vcs_checkout_and_version_metadata
    @formula.stable = vcs_resource
    collect
    manifest = JSON.parse((@output/"source-manifest.json").read)
    entry = manifest["packages"].first["artifacts"].find { |item| item["role"] == "source" }
    assert_equal @formula.stable.specs[:revision], entry["git_revision"]
    files = MacosSources.command("tar", "-tzf", (@output/entry["path"]).to_s)
    assert_includes files, "./source.c"
    assert_includes files, "./.git/HEAD"
  end

  def test_rejects_wrong_vcs_checkout
    @formula.stable = vcs_resource
    @formula.stable.specs[:revision] = "a" * 40
    assert_failed(/VCS revision mismatch/)
  end

  def test_rejects_dirty_vcs_snapshot
    @formula.stable = vcs_resource
    (@formula.stable.path/"source.c").write("altered")
    assert_failed(/Dirty VCS source/)
  end

  def test_rejects_external_patch_without_integrity_pin
    patch = resource("unsafe-patch")
    patch.checksum = nil
    @formula.patchlist << External.new(patch, :p1, [])
    assert_failed(/Unpinned source: patch/)
  end

  def test_resumes_transient_native_download_errors_with_a_bounded_budget
    attempts = 0
    item = resource("retry")
    item.define_singleton_method(:fetch) do |**_options|
      attempts += 1
      raise DownloadError, "partial transfer" if attempts < 3

      path
    end
    assert_equal item.path, MacosSources.fetch_resource(item)
    assert_equal 3, attempts
  end

  def test_does_not_retry_other_download_failures
    attempts = 0
    item = resource("failure")
    item.define_singleton_method(:fetch) do |**_options|
      attempts += 1
      raise RuntimeError, "integrity mismatch"
    end
    assert_raises(RuntimeError) { MacosSources.fetch_resource(item) }
    assert_equal 1, attempts
  end
end
