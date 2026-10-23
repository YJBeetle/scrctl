#!/bin/bash
# Sign only relocated package copies. Secrets are supplied by the CI environment.
set -euo pipefail
root=${1:?Usage: sign_macos_package.sh INSTALL_DIRECTORY [EXECUTABLE_NAME]}
main_binary=${2:-scrctl}
: "${MACOS_CERTIFICATE_P12_BASE64:?Missing certificate}"
: "${MACOS_CERTIFICATE_PASSWORD:?Missing certificate password}"
: "${SIGNING_IDENTITY:?Missing signing identity}"
: "${SIGNING_TEAM_ID:?Missing team ID}"
[[ "$SIGNING_IDENTITY" == 'Developer ID Application: '* ]] || { echo 'Expected Developer ID Application identity' >&2; exit 1; }
task_dir=$(mktemp -d)
keychain="$task_dir/signing.keychain-db"
keychain_password=$(openssl rand -hex 24)
original_keychains=()
while IFS= read -r entry; do
    entry="${entry#*\"}"; entry="${entry%\"*}"
    [[ -z "$entry" ]] || original_keychains+=("$entry")
done < <(security list-keychains -d user)
cleanup() {
    security list-keychains -d user -s "${original_keychains[@]}" >/dev/null 2>&1 || true
    security delete-keychain "$keychain" >/dev/null 2>&1 || true
    rm -rf "$task_dir"
}
trap cleanup EXIT
printf '%s' "$MACOS_CERTIFICATE_P12_BASE64" | /usr/bin/base64 -D > "$task_dir/certificate.p12"
security create-keychain -p "$keychain_password" "$keychain"
security list-keychains -d user -s "$keychain" "${original_keychains[@]}"
security set-keychain-settings -lut 21600 "$keychain"
security unlock-keychain -p "$keychain_password" "$keychain"
security import "$task_dir/certificate.p12" -k "$keychain" -P "$MACOS_CERTIFICATE_PASSWORD" -T /usr/bin/codesign
echo "Authorizing the imported signing key"
security set-key-partition-list -S apple-tool:,apple:,codesign: -s -k "$keychain_password" "$keychain" >/dev/null
# Resolve the exact certificate name to a fingerprint before signing. Certificate
# names/hashes are public; never print the P12, its password or private key.
echo "Checking imported code-signing identities"
identities=$(security find-identity -v -p codesigning "$keychain")
signing_hash=$(printf '%s\n' "$identities" | awk -v identity="$SIGNING_IDENTITY" '
    index($0, "\"" identity "\"") { print $2; exit }')
if [[ -z "$signing_hash" ]]; then
    printf '%s\n' "$identities" >&2
    echo "No valid identity matching SIGNING_IDENTITY in the imported P12" >&2
    exit 1
fi
echo "Signing package with verified Developer ID identity"
while IFS= read -r -d '' binary; do
    file -b "$binary" | grep -q 'Mach-O' || continue
    if [[ "$binary" == "$root/bin/$main_binary" ]]; then
        codesign --force --sign "$signing_hash" --keychain "$keychain" --timestamp --options runtime "$binary"
    else
        codesign --force --sign "$signing_hash" --keychain "$keychain" --timestamp "$binary"
    fi
    codesign --verify --strict --verbose=2 "$binary"
    details=$(codesign -dvv "$binary" 2>&1)
    [[ "$details" == *"TeamIdentifier=$SIGNING_TEAM_ID"* ]] || { echo "Wrong signing team: $binary" >&2; exit 1; }
done < <(find "$root" -type f -print0)
# Do not accept a package missing its main executable.
codesign --verify --strict "$root/bin/$main_binary"
codesign -dvv "$root/bin/$main_binary" 2>&1 | grep -q 'flags=.*runtime'
