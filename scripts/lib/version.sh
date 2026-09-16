#!/usr/bin/env bash

version_is_greater() {
  local candidate="$1"
  local allowed="$2"
  awk -v candidate="$candidate" -v allowed="$allowed" 'BEGIN {
    candidate_count = split(candidate, candidate_parts, ".")
    allowed_count = split(allowed, allowed_parts, ".")
    count = candidate_count > allowed_count ? candidate_count : allowed_count
    for (part_index = 1; part_index <= count; ++part_index) {
      candidate_part = candidate_parts[part_index] + 0
      allowed_part = allowed_parts[part_index] + 0
      if (candidate_part > allowed_part) exit 0
      if (candidate_part < allowed_part) exit 1
    }
    exit 1
  }'
}
