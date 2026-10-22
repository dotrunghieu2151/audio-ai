
#!/usr/bin/env bash
#
# Cross-platform vcpkg installer.
# Supports Linux, macOS, WSL, and Git Bash on Windows.
#
# Usage:
#   ./scripts/install-vcpkg.sh --version 2025.01.13
#   ./scripts/install-vcpkg.sh --version <40-character-commit-sha>
#   ./scripts/install-vcpkg.sh --version 2025.01.13 --root /opt/vcpkg
#   ./scripts/install-vcpkg.sh --help
#
# Environment overrides:
#   VCPKG_ROOT
#   VCPKG_CONFIG_FILE
#   VCPKG_REPOSITORY
#
# Requirements: Bash, Git, and jq
#
set -Eeuo pipefail

readonly DEFAULT_REPOSITORY="https://github.com/microsoft/vcpkg"

VERSION=""
ROOT="${VCPKG_ROOT:-}"
CONFIG="${VCPKG_CONFIG_FILE:-}"
REPOSITORY="${VCPKG_REPOSITORY:-$DEFAULT_REPOSITORY}"

log()  { printf '[vcpkg] %s\n' "$*"; }
die()  { printf '[vcpkg] ERROR: %s\n' "$*" >&2; exit 1; }

usage() {
    cat <<'EOF'
Usage:
  install-vcpkg.sh --version <tag-or-commit> [options]

Options:
  --version VERSION    Release tag or full 40-character commit SHA
  --root PATH          Installation directory
  --config PATH        Path to vcpkg-configuration.json
  --repository URL     vcpkg Git repository URL
  -h, --help           Show help

Environment variables:
  VCPKG_ROOT
  VCPKG_CONFIG_FILE
  VCPKG_REPOSITORY

Examples:
  ./scripts/install-vcpkg.sh --version 2025.01.13
  ./scripts/install-vcpkg.sh --version 2025.01.13 --root "$HOME/.local/share/vcpkg"
EOF
}

while (($#)); do
    case "$1" in
        --version)
            (($# >= 2)) || die "--version requires a value"
            VERSION="$2"; shift 2 ;;
        --root)
            (($# >= 2)) || die "--root requires a path"
            ROOT="$2"; shift 2 ;;
        --config)
            (($# >= 2)) || die "--config requires a path"
            CONFIG="$2"; shift 2 ;;
        --repository)
            (($# >= 2)) || die "--repository requires a URL"
            REPOSITORY="$2"; shift 2 ;;
        -h|--help)
            usage; exit 0 ;;
        *)
            die "Unknown argument: $1" ;;
    esac
done

[[ -n "$VERSION" ]] || {
    usage >&2
    die "Specify --version"
}

command -v git >/dev/null 2>&1 || die "Git is required"
command -v jq  >/dev/null 2>&1 || die "jq is required"

# Resolve paths relative to this script's project root by default.
SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd -P)"
PROJECT_ROOT="$(cd -- "$SCRIPT_DIR/.." && pwd -P)"

ROOT="${ROOT:-$PROJECT_ROOT/.vcpkg}"
CONFIG="${CONFIG:-$PROJECT_ROOT/vcpkg-configuration.json}"

mkdir -p "$(dirname -- "$ROOT")"
ROOT="$(cd -- "$(dirname -- "$ROOT")" && pwd -P)/$(basename -- "$ROOT")"

[[ -f "$CONFIG" ]] || die "Configuration file not found: $CONFIG"
jq -e 'type == "object"' "$CONFIG" >/dev/null ||
    die "Invalid JSON in $CONFIG"

KIND="$(jq -er '.["default-registry"].kind' "$CONFIG")" ||
    die "Missing default-registry.kind"

BASELINE="$(jq -er '.["default-registry"].baseline' "$CONFIG")" ||
    die "Missing default-registry.baseline"

[[ "$BASELINE" =~ ^[[:xdigit:]]{40}$ ]] ||
    die "Baseline must be a full 40-character commit SHA"

BASELINE="${BASELINE,,}"

case "$KIND" in
    builtin)
        # Built-in registry uses the vcpkg repository.
        ;;
    git)
        REGISTRY_REPOSITORY="$(
            jq -er '.["default-registry"].repository' "$CONFIG"
        )" || die "Missing default-registry.repository"

        normalize_url() {
            local url="${1%/}"
            printf '%s' "${url%.git}"
        }

        [[ "$(normalize_url "$REGISTRY_REPOSITORY")" == \
           "$(normalize_url "$REPOSITORY")" ]] ||
            die "The Git registry is a different repository. Its baseline cannot verify this vcpkg checkout."
        ;;
    *)
        die "Unsupported registry kind: $KIND"
        ;;
esac

# Avoid accidentally operating on an unrelated directory.
if [[ -e "$ROOT" && ! -d "$ROOT/.git" ]]; then
    die "$ROOT exists but is not a Git checkout"
fi

log "Requested version: $VERSION"
log "Repository: $REPOSITORY"
log "Root: $ROOT"
log "Configuration baseline: $BASELINE"

if [[ ! -d "$ROOT/.git" ]]; then
    log "Cloning vcpkg..."
    git clone "$REPOSITORY" "$ROOT" ||
        die "Git clone failed"
fi

# Fetch all tags and history needed for version/baseline verification.
git -C "$ROOT" fetch --tags origin ||
    die "Git fetch failed; refusing to use stale repository data"

# Resolve a tag or full commit to its exact commit.
if [[ "$VERSION" =~ ^[[:xdigit:]]+$ ]]; then
    [[ ${#VERSION} -eq 40 ]] ||
        die "A commit must be a full 40-character SHA"
    REQUESTED_REF="$VERSION"
else
    REQUESTED_REF="refs/tags/$VERSION"
fi

TARGET_COMMIT="$(
    git -C "$ROOT" rev-parse --verify "${REQUESTED_REF}^{commit}" 2>/dev/null
)" || die "Cannot resolve '$VERSION' to a commit"

TARGET_COMMIT="${TARGET_COMMIT,,}"

log "Resolved commit: $TARGET_COMMIT"

# Verify the configured baseline is a commit in this repository.
git -C "$ROOT" cat-file -e "${BASELINE}^{commit}" 2>/dev/null ||
    die "Baseline $BASELINE is not present in the fetched repository history"

# Deliberately require the configured baseline to match the selected release.
# This preserves the requested policy that the config verifies the release hash.
if [[ "$BASELINE" != "$TARGET_COMMIT" ]]; then
    die "Version '$VERSION' resolves to $TARGET_COMMIT, but vcpkg-configuration.json specifies baseline $BASELINE. Choose the matching release or update the configuration."
fi

# Detect native Windows shells versus Unix-like environments.
case "$(uname -s)" in
    MINGW*|MSYS*)
        PLATFORM="windows"
        VCPKG_EXE="$ROOT/vcpkg.exe"
        BOOTSTRAP="$ROOT/bootstrap-vcpkg.bat"
        ;;
    *)
        PLATFORM="unix"
        VCPKG_EXE="$ROOT/vcpkg"
        BOOTSTRAP="$ROOT/bootstrap-vcpkg.sh"
        ;;
esac

CURRENT_COMMIT="$(
    git -C "$ROOT" rev-parse HEAD
)" || die "Cannot read current checkout"

CURRENT_COMMIT="${CURRENT_COMMIT,,}"

# Fast path: exact version, verified baseline, and usable executable.
if [[ "$CURRENT_COMMIT" == "$TARGET_COMMIT" &&
      -x "$VCPKG_EXE" ]]; then
    log "Requested version already installed and verified."
    exit 0
fi

# Never discard developer modifications.
if [[ -n "$(git -C "$ROOT" status --porcelain)" ]]; then
    die "Working tree has local changes. Commit, stash, or remove them before switching versions."
fi

if [[ "$CURRENT_COMMIT" != "$TARGET_COMMIT" ]]; then
    log "Checking out $TARGET_COMMIT"
    git -C "$ROOT" checkout --detach "$TARGET_COMMIT" ||
        die "Git checkout failed"
fi

[[ -f "$BOOTSTRAP" ]] ||
    die "Bootstrap script not found: $BOOTSTRAP"

log "Bootstrapping vcpkg..."

if [[ "$PLATFORM" == "windows" ]]; then
    command -v cmd.exe >/dev/null 2>&1 ||
        die "cmd.exe is required under Git Bash"

    WIN_BOOTSTRAP="$(cygpath -w "$BOOTSTRAP")"
    WIN_ROOT="$(cygpath -w "$ROOT")"

    (
        cd "$ROOT"
        cmd.exe /d /s /c "\"$WIN_BOOTSTRAP\" -disableMetrics"
    ) || die "vcpkg bootstrap failed"
else
    chmod +x "$BOOTSTRAP"
    (
        cd "$ROOT"
        "$BOOTSTRAP" -disableMetrics
    ) || die "vcpkg bootstrap failed"
fi

[[ -f "$VCPKG_EXE" ]] ||
    die "Bootstrap finished but vcpkg executable is missing"

log "Installation successful"
log "Version: $VERSION"
log "Commit: $TARGET_COMMIT"
log "Executable: $VCPKG_EXE"

