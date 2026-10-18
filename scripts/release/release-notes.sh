#!/usr/bin/env bash

set -euo pipefail

usage() {
  cat <<'EOF'
Usage:
  ./scripts/release/release-notes.sh --tag <tag> [options]

Options:
  --tag <tag>          Tag to generate notes for (e.g. vX.Y.Z). Required.
  --version <version>  Version used in artifact names (default: tag without leading v).
  --repo <owner/repo>  GitHub repository for compare links (optional).
  --range <A..B>       Explicit git range. If omitted, uses previous tag..tag.
  --output <file>      Output markdown path. If omitted, prints to stdout.
EOF
}

die() {
  echo "[release-notes] $*" >&2
  exit 1
}

# Turns a commit description into a sentence: single spaces and a capital
# first letter.
sentence() {
  local -a words
  read -r -a words <<<"$1"
  local text="${words[*]}"
  local first
  first="$(printf '%s' "${text:0:1}" | tr '[:lower:]' '[:upper:]')"
  printf '%s' "${first}${text:1}"
}

# Prints "1 fix" or "3 fixes".
count_phrase() {
  if (($1 == 1)); then
    printf '1 %s' "$2"
  else
    printf '%s %s' "$1" "$3"
  fi
}

# Joins words as "A", "A and B" or "A, B and C".
join_phrases() {
  local result="" index
  for ((index = 1; index <= $#; index++)); do
    if ((index == 1)); then
      result="${!index}"
    elif ((index == $#)); then
      result+=" and ${!index}"
    else
      result+=", ${!index}"
    fi
  done
  printf '%s' "$result"
}

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
cd "$REPO_ROOT"

TAG=""
VERSION=""
REPO=""
RANGE=""
OUTPUT=""

while (($#)); do
  case "$1" in
  --tag)
    (($# >= 2)) || die "Missing value for --tag"
    TAG="$2"
    shift 2
    ;;
  --version)
    (($# >= 2)) || die "Missing value for --version"
    VERSION="$2"
    shift 2
    ;;
  --repo)
    (($# >= 2)) || die "Missing value for --repo"
    REPO="$2"
    shift 2
    ;;
  --range)
    (($# >= 2)) || die "Missing value for --range"
    RANGE="$2"
    shift 2
    ;;
  --output)
    (($# >= 2)) || die "Missing value for --output"
    OUTPUT="$2"
    shift 2
    ;;
  -h | --help)
    usage
    exit 0
    ;;
  *)
    die "Unknown argument: $1"
    ;;
  esac
done

[[ -n "$TAG" ]] || {
  usage
  die "--tag is required"
}

git rev-parse --verify "${TAG}^{commit}" >/dev/null 2>&1 ||
  die "Tag not found (or not resolvable as commit): ${TAG}"

if [[ -z "$VERSION" ]]; then
  VERSION="${TAG#v}"
fi

PREV_TAG=""
if [[ -z "$RANGE" ]]; then
  PREV_TAG="$(git describe --tags --abbrev=0 "${TAG}^" 2>/dev/null || true)"
  if [[ -n "$PREV_TAG" ]]; then
    RANGE="${PREV_TAG}..${TAG}"
  else
    RANGE="${TAG}"
  fi
fi

AREAS_FILE="${SCRIPT_DIR}/release-note-areas.tsv"
AREA_NAMES=()
AREA_SCOPES=()
INTERNAL_SCOPES=" "

# Sets found_area to an area index, to "internal", or to "" for a scope that
# release-note-areas.tsv does not list.
find_area() {
  local scope="$1" index
  found_area=""
  [[ -n "$scope" ]] || return 0
  if [[ "$INTERNAL_SCOPES" == *" ${scope} "* ]]; then
    found_area="internal"
    return 0
  fi
  for index in "${!AREA_SCOPES[@]}"; do
    if [[ "${AREA_SCOPES[$index]}" == *" ${scope} "* ]]; then
      found_area="$index"
      return 0
    fi
  done
}

[[ -r "$AREAS_FILE" ]] || die "Cannot read ${AREAS_FILE}"
line_number=0
while IFS= read -r line || [[ -n "$line" ]]; do
  line_number=$((line_number + 1))
  [[ -z "$line" || "$line" == \#* ]] && continue
  [[ "$line" == *$'\t'* ]] ||
    die "${AREAS_FILE}:${line_number}: expected an area, a tab and its scopes"
  area_name="${line%%$'\t'*}"
  read -r -a area_scope_list <<<"${line#*$'\t'}"
  [[ -n "$area_name" && ${#area_scope_list[@]} -gt 0 ]] ||
    die "${AREAS_FILE}:${line_number}: an area needs a name and scopes"
  for scope in "${area_scope_list[@]}"; do
    find_area "$scope"
    [[ -z "$found_area" ]] ||
      die "${AREAS_FILE}:${line_number}: scope '${scope}' is listed twice"
  done
  if [[ "$area_name" == "internal" ]]; then
    INTERNAL_SCOPES+="${area_scope_list[*]} "
  else
    AREA_NAMES+=("$area_name")
    AREA_SCOPES+=(" ${area_scope_list[*]} ")
  fi
done <"$AREAS_FILE"
((${#AREA_NAMES[@]} > 0)) || die "${AREAS_FILE} lists no areas"

# User-visible changes are kept per area; the last slot is Other.
OTHER_AREA=${#AREA_NAMES[@]}
AREA_NAMES+=("Other")
FEAT_ITEMS=()
FIX_ITEMS=()
PERF_ITEMS=()
AREA_TOTALS=()
for index in "${!AREA_NAMES[@]}"; do
  FEAT_ITEMS[index]=""
  FIX_ITEMS[index]=""
  PERF_ITEMS[index]=""
  AREA_TOTALS[index]=0
done
feat_count=0
fix_count=0
perf_count=0
breaking_items=""
maintenance_items=""
maintenance_count=0
unlisted_scopes=" "
VERSION_BUMP_PATTERN='^[[:space:]]*[Bb]ump version'

commits_raw="$(
  git log --no-merges --reverse \
    --pretty=tformat:'%H%x1f%s%x1f%b%x1e' \
    "$RANGE"
)"

if [[ -n "$commits_raw" ]]; then
  while IFS= read -r -d $'\x1e' record; do
    [[ -n "$record" ]] || continue
    record="${record#$'\n'}"
    if [[ "$record" != *$'\x1f'* ]]; then
      continue
    fi
    sha="${record%%$'\x1f'*}"
    rest="${record#*$'\x1f'}"
    if [[ "$rest" != *$'\x1f'* ]]; then
      continue
    fi
    subject="${rest%%$'\x1f'*}"
    body="${rest#*$'\x1f'}"

    sha="${sha//$'\r'/}"
    subject="${subject//$'\r'/}"
    short_sha="${sha:0:8}"

    type=""
    scope=""
    bang=""
    desc="$subject"
    if [[ "$subject" == *:* ]]; then
      prefix="${subject%%:*}"
      raw_desc="${subject#*:}"
      if [[ "$prefix" =~ ^([A-Za-z]+)(\((.*)\))?(!)?$ ]]; then
        type="$(printf '%s' "${BASH_REMATCH[1]}" | tr '[:upper:]' '[:lower:]')"
        # A commit with several scopes is filed under the first.
        scope="${BASH_REMATCH[3]%%,*}"
        scope="${scope// /}"
        bang="${BASH_REMATCH[4]}"
        desc="$raw_desc"
      fi
    fi
    item="$(sentence "$desc") (${short_sha})"

    if [[ -n "$bang" || "$body" == *"BREAKING CHANGE:"* ||
      "$body" == *"BREAKING-CHANGE:"* ]]; then
      breaking_note=""
      while IFS= read -r line; do
        # git puts a space before the body in this log format.
        line="${line#"${line%%[![:space:]]*}"}"
        if [[ "$line" =~ ^BREAKING[[:space:]-]CHANGE:[[:space:]]*(.+)$ ]]; then
          breaking_note="${BASH_REMATCH[1]}"
          break
        fi
      done <<<"$body"
      if [[ -n "$breaking_note" ]]; then
        breaking_items+="- $(sentence "$breaking_note") (${short_sha})"$'\n'
      else
        breaking_items+="- ${item}"$'\n'
      fi
    fi

    case "$type" in
    feat | fix | perf)
      find_area "$scope"
      if [[ "$found_area" != "internal" ]]; then
        if [[ -z "$found_area" ]]; then
          found_area=$OTHER_AREA
          if [[ "$unlisted_scopes" != *" ${scope:-(none)} "* ]]; then
            unlisted_scopes+="${scope:-(none)} "
          fi
        fi
        case "$type" in
        feat)
          FEAT_ITEMS[found_area]+="- **${AREA_NAMES[$found_area]}:** ${item}"$'\n'
          feat_count=$((feat_count + 1))
          ;;
        fix)
          FIX_ITEMS[found_area]+="- ${item}"$'\n'
          fix_count=$((fix_count + 1))
          ;;
        perf)
          PERF_ITEMS[found_area]+="- ${item}"$'\n'
          perf_count=$((perf_count + 1))
          ;;
        esac
        AREA_TOTALS[found_area]=$((AREA_TOTALS[found_area] + 1))
        continue
      fi
      ;;
    "")
      echo "[release-notes] ${short_sha} has no Conventional Commits type; listed under Maintenance" >&2
      ;;
    esac
    # The version bump is the release itself, not a change in it.
    if [[ "$desc" =~ $VERSION_BUMP_PATTERN ]]; then
      continue
    fi
    maintenance_items+="- ${item}"$'\n'
    maintenance_count=$((maintenance_count + 1))
  done < <(printf '%s' "$commits_raw")
fi

for scope in $unlisted_scopes; do
  echo "[release-notes] scope '${scope}' is not in release-note-areas.tsv; its changes are listed under Other" >&2
done

# Prints the fixes or performance changes with one heading per area.
print_by_area() {
  local title="$1" index items
  printf '## %s\n\n' "$title"
  for index in "${!AREA_NAMES[@]}"; do
    if [[ "$title" == "Fixes" ]]; then
      items="${FIX_ITEMS[$index]}"
    else
      items="${PERF_ITEMS[$index]}"
    fi
    [[ -n "$items" ]] || continue
    printf '### %s\n\n%s\n' "${AREA_NAMES[$index]}" "$items"
  done
}

summary=()
if ((feat_count > 0)); then
  summary+=("$(count_phrase "$feat_count" "new feature" "new features")")
fi
if ((fix_count > 0)); then
  summary+=("$(count_phrase "$fix_count" "fix" "fixes")")
fi
if ((perf_count > 0)); then
  summary+=("$(count_phrase "$perf_count" "performance improvement" "performance improvements")")
fi
# Up to three areas with the most changes, Other aside.
busiest=()
chosen=" "
for _ in 1 2 3; do
  best=""
  for ((index = 0; index < OTHER_AREA; index++)); do
    [[ "$chosen" != *" ${index} "* ]] || continue
    ((AREA_TOTALS[index] > 0)) || continue
    if [[ -z "$best" ]] || ((AREA_TOTALS[index] > AREA_TOTALS[best])); then
      best=$index
    fi
  done
  [[ -n "$best" ]] || break
  chosen+="${best} "
  busiest+=("**${AREA_NAMES[$best]}**")
done

{
  cat <<EOF
<!-- RELEASE OVERVIEW: write the release name and a short introduction before publishing. Everything below is generated from the commits. -->
# Release name (${TAG})

_Add a short introduction for this release._

<!-- END RELEASE OVERVIEW -->

EOF

  if ((${#summary[@]} > 0)); then
    line="$(join_phrases "${summary[@]}")"
    if ((${#busiest[@]} > 0)); then
      line+=", mostly in $(join_phrases "${busiest[@]}")"
    fi
    printf '%s.\n\n' "$line"
  fi

  if [[ -n "$breaking_items" ]]; then
    printf '## Breaking changes\n\n%s\n' "$breaking_items"
  fi
  if ((feat_count > 0)); then
    printf "## What's new\n\n"
    for index in "${!AREA_NAMES[@]}"; do
      printf '%s' "${FEAT_ITEMS[$index]}"
    done
    printf '\n'
  fi
  if ((fix_count > 0)); then
    print_by_area "Fixes"
  fi
  if ((perf_count > 0)); then
    print_by_area "Performance"
  fi

  if ((maintenance_count > 0)); then
    cat <<EOF
<details>
<summary><strong>Maintenance</strong>: $(count_phrase "$maintenance_count" "behind-the-scenes change" "behind-the-scenes changes") to code, tests, builds, releases and documentation</summary>

${maintenance_items}
</details>

EOF
  fi

  cat <<EOF
<details>
<summary><strong>Download and install</strong></summary>

## Verify downloads

Verify the download first. \`SHA256SUMS.txt\` is attached to this release:

\`\`\`bash
shasum -a 256 -c SHA256SUMS.txt --ignore-missing  # macOS
sha256sum -c SHA256SUMS.txt --ignore-missing      # Linux
\`\`\`

On Windows, compare the hash that PowerShell prints with the file's line in \`SHA256SUMS.txt\`:

\`\`\`powershell
Get-FileHash -Algorithm SHA256 .\\OpenSCP-${VERSION}-x64-setup.exe
\`\`\`

Checksums catch a damaged download, not a replaced one. To confirm that a file was built by this repository's release workflow, use the [GitHub CLI](https://cli.github.com/):

\`\`\`bash
gh attestation verify OpenSCP-${VERSION}-x86_64.AppImage --repo luiscuellar31/openscp
\`\`\`

Any other attached file works the same way, such as \`OpenSCP-${VERSION}-x64-setup.exe\`.

### macOS

Download \`OpenSCP-${VERSION}-arm64-UNSIGNED.dmg\` on Apple Silicon or \`OpenSCP-${VERSION}-x86_64-UNSIGNED.dmg\` on Intel, open it, and drag OpenSCP onto Applications.

OpenSCP is not signed with an Apple Developer ID, so macOS blocks the first launch and reports that it cannot check the app for malicious software. To allow it, open **System Settings → Privacy & Security**, scroll down to the Security section, click **Open Anyway** next to the message about OpenSCP, and confirm. The DMG ships these steps as \`README.txt\`.

### Linux

AppImage, which runs on most distributions:

\`\`\`bash
chmod +x OpenSCP-${VERSION}-x86_64.AppImage
./OpenSCP-${VERSION}-x86_64.AppImage
\`\`\`

Flatpak bundle:

\`\`\`bash
flatpak remote-add --user --if-not-exists flathub https://flathub.org/repo/flathub.flatpakrepo
flatpak install --user ./OpenSCP-x86_64.flatpak
flatpak run io.github.luiscuellar31.openscp
\`\`\`

Replace \`x86_64\` with \`aarch64\` on ARM64 machines.

### Windows

Download \`OpenSCP-${VERSION}-x64-setup.exe\` and run it. It installs for the current user without administrator rights, offers an installation for all users, and updates itself. \`OpenSCP-${VERSION}-x64.msi\` installs for all users, for deployment tools such as Intune or Group Policy, and is updated by installing a newer MSI. Both need 64-bit Windows 10 version 1809 or later.

OpenSCP is not code-signed, so SmartScreen may warn before the first run. To allow it, click **More info**, then **Run anyway**.

</details>

EOF

  if [[ -n "$PREV_TAG" && -n "$REPO" ]]; then
    echo "**Full comparison:** [\`${PREV_TAG}...${TAG}\`](https://github.com/${REPO}/compare/${PREV_TAG}...${TAG})"
  elif [[ -n "$PREV_TAG" ]]; then
    echo "**Commit range:** \`${PREV_TAG}..${TAG}\`"
  else
    echo "**Commit range:** \`${TAG}\`"
  fi
} >"${OUTPUT:-/dev/stdout}"
