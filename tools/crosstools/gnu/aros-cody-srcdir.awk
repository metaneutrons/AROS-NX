# Map libcody's diagnostic source prefix with declared file/macro maps.
# Unlike recorded checksum options, this changes only the diagnostic string.
function fail(message) {
  print "AROS GCC libcody SRCDIR: " message > "/dev/stderr"
  exit 2
}

BEGIN {
  if (ENVIRON["AROS_GCC_RELEASE_LAYOUT"] != "yes")
    fail("release layout is not enabled")

  source = ENVIRON["AROS_GCC_RELEASE_CODY_SRCDIR"]
  if (source == "" || source !~ /^\// || source == "/" || source ~ /[[:cntrl:]]/)
    fail("libcody source directory must be a non-root absolute path")

  declared = ENVIRON["AROS_GCC_RELEASE_PREFIX_MAPS"]
  if (declared == "")
    fail("no declared prefix maps")

  count = split(declared, maps, /[ \t]+/)
  for (i = 1; i <= count; i++) {
    token = maps[i]
    if (token == "" || token ~ /[[:cntrl:]]/ ||
        token !~ /^-f(file|debug|macro)-prefix-map=\/[^= \t]+=\/[^= \t]+$/)
      fail("invalid declared prefix map")

    field_count = split(token, fields, "=")
    if (field_count != 3 || fields[2] == "/" || fields[3] == "/")
      fail("filesystem-root or malformed prefix map is not permitted")
    if (index(fields[2], "'") || index(fields[2], "\"") || index(fields[2], "\\") ||
        index(fields[3], "'") || index(fields[3], "\"") || index(fields[3], "\\"))
      fail("quoted or backslash prefix-map paths are not supported")
    if (token in seen)
      fail("duplicate declared prefix map")
    seen[token] = 1
    key = fields[1] SUBSEP fields[2]
    if (key in destination && destination[key] != fields[3])
      fail("conflicting destination for a prefix-map kind and source")
    destination[key] = fields[3]
    kinds[i] = fields[1]
    old[i] = fields[2]
    new[i] = fields[3]
  }

  # Compiler implementations can resolve overlapping maps differently.  Only
  # accept a diagnostic namespace when every matching file/macro map agrees.
  # Matching is deliberately textual: no path-boundary or realpath rules.
  for (i = 1; i <= count; i++) {
    if (kinds[i] == "-fdebug-prefix-map")
      continue
    if (substr(source, 1, length(old[i])) == old[i]) {
      candidate = new[i] substr(source, length(old[i]) + 1)
      if (candidate == "" || candidate == "/" || candidate ~ /[[:cntrl:]]/ ||
          index(candidate, "'") || index(candidate, "\"") || index(candidate, "\\"))
        fail("mapped libcody source directory is unsafe for the diagnostic macro")
      if (matched && candidate != mapped)
        fail("ambiguous diagnostic namespace: matching file/macro maps disagree")
      mapped = candidate
      matched = 1
    }
  }

  if (matched) {
    print mapped
    exit 0
  }

  fail("no file or macro prefix map matches libcody source directory")
}
