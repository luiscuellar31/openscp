#!/usr/bin/env bash

set -euo pipefail

usage() {
  cat <<'EOF'
Usage:
  scripts/generate_release_notes.sh --tag <tag> [options]

Options:
  --tag <tag>          Tag to generate notes for (e.g. v0.8.1). Required.
  --version <version>  Version label to show in notes (default: tag without leading v).
  --repo <owner/repo>  GitHub repository for compare links (optional).
  --range <A..B>       Explicit git range. If omitted, uses previous tag..tag.
  --output <file>      Output markdown path. If omitted, prints to stdout.
EOF
}

die() {
  echo "[release-notes] $*" >&2
  exit 1
}

append_item() {
  local var_name="$1"
  local text="$2"
  printf -v "$var_name" '%s- %s\n' "${!var_name}" "$text"
}

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

commits_raw="$(
  git log --no-merges --reverse \
    --pretty=tformat:'%H%x1f%s%x1f%b%x1e' \
    "$RANGE"
)"

breaking_items=""
feat_items=""
fix_items=""
perf_items=""
refactor_items=""
docs_items=""
maintenance_items=""
other_items=""

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
      raw_desc="${raw_desc#"${raw_desc%%[![:space:]]*}"}"
      if [[ "$prefix" =~ ^([A-Za-z]+)(\(.*\))?(!)?$ ]]; then
        type="$(printf '%s' "${BASH_REMATCH[1]}" | tr '[:upper:]' '[:lower:]')"
        scope="${BASH_REMATCH[2]}"
        bang="${BASH_REMATCH[3]}"
        desc="$raw_desc"
      fi
    fi

    if [[ -n "$scope" ]]; then
      scope_name="${scope#(}"
      scope_name="${scope_name%)}"
      desc="${scope_name}: ${desc}"
    fi

    entry="${desc} (${short_sha})"

    is_breaking=0
    breaking_note=""
    if [[ -n "$bang" ]]; then
      is_breaking=1
    fi
    if [[ "$body" == *"BREAKING CHANGE:"* || "$body" == *"BREAKING-CHANGE:"* ]]; then
      is_breaking=1
      while IFS= read -r line; do
        if [[ "$line" =~ ^BREAKING[[:space:]-]CHANGE:[[:space:]]*(.+)$ ]]; then
          breaking_note="${BASH_REMATCH[1]}"
          break
        fi
      done <<<"$body"
    fi
    if ((is_breaking)); then
      if [[ -n "$breaking_note" ]]; then
        append_item breaking_items "${breaking_note} (${short_sha})"
      else
        append_item breaking_items "$entry"
      fi
    fi

    case "$type" in
    feat)
      append_item feat_items "$entry"
      ;;
    fix)
      append_item fix_items "$entry"
      ;;
    perf)
      append_item perf_items "$entry"
      ;;
    refactor)
      append_item refactor_items "$entry"
      ;;
    docs)
      append_item docs_items "$entry"
      ;;
    build | ci | chore | style | test | revert)
      append_item maintenance_items "$entry"
      ;;
    "")
      append_item other_items "${subject} (${short_sha})"
      ;;
    *)
      append_item other_items "$entry"
      ;;
    esac
  done < <(printf '%s' "$commits_raw")
fi

date_iso="$(date -u +%Y-%m-%d)"

{
  echo "# OpenSCP v${VERSION}"
  echo
  echo "- Tag: \`${TAG}\`"
  echo "- Date (UTC): ${date_iso}"
  if [[ -n "$PREV_TAG" ]]; then
    echo "- Range: \`${PREV_TAG}..${TAG}\`"
    if [[ -n "$REPO" ]]; then
      echo "- Compare: https://github.com/${REPO}/compare/${PREV_TAG}...${TAG}"
    fi
  else
    echo "- Range: \`${TAG}\` (first tagged release or previous tag not found)"
  fi
  echo

  echo "## Breaking Changes"
  if [[ -n "$breaking_items" ]]; then
    printf '%s' "$breaking_items"
  else
    echo "- None."
  fi
  echo

  echo "## Features"
  if [[ -n "$feat_items" ]]; then
    printf '%s' "$feat_items"
  else
    echo "- None."
  fi
  echo

  echo "## Fixes"
  if [[ -n "$fix_items" ]]; then
    printf '%s' "$fix_items"
  else
    echo "- None."
  fi
  echo

  echo "## Performance"
  if [[ -n "$perf_items" ]]; then
    printf '%s' "$perf_items"
  else
    echo "- None."
  fi
  echo

  echo "## Refactors"
  if [[ -n "$refactor_items" ]]; then
    printf '%s' "$refactor_items"
  else
    echo "- None."
  fi
  echo

  echo "## Documentation"
  if [[ -n "$docs_items" ]]; then
    printf '%s' "$docs_items"
  else
    echo "- None."
  fi
  echo

  echo "## Maintenance"
  if [[ -n "$maintenance_items" ]]; then
    printf '%s' "$maintenance_items"
  else
    echo "- None."
  fi
  echo

  echo "## Other Changes"
  if [[ -n "$other_items" ]]; then
    printf '%s' "$other_items"
  else
    echo "- None."
  fi
} >"${OUTPUT:-/dev/stdout}"
