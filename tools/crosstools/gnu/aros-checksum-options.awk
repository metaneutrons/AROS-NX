# Canonicalize only declared producer prefix-map inputs in GCC's PCH record.
# Actual compilation flags and genchecksum's object/library inputs are unchanged.
function fail(message) {
  print "AROS GCC checksum options: " message > "/dev/stderr"
  failed = 1
  exit 2
}

BEGIN {
  count = split(ENVIRON["AROS_GCC_RELEASE_PREFIX_MAPS"], maps, /[ \t]+/)
  for (i = 1; i <= count; i++) {
    token = maps[i]
    if (token == "")
      continue
    if (token ~ /[[:cntrl:]]/ || token !~ /^-f(file|debug|macro)-prefix-map=\/[^= \t]+=\/[^= \t]+$/)
      fail("invalid declared prefix map")
    split(token, fields, "=")
    if (fields[2] == "/" || fields[3] == "/")
      fail("filesystem-root prefix map is not permitted")
    if (token in canonical)
      fail("duplicate declared prefix map")
    key = fields[1] "=" fields[2]
    if (key in destination)
      fail("conflicting declared prefix map")
    destination[key] = fields[3]
    canonical[token] = fields[1] "=/aros-recorded-root=" fields[3]
  }
}

{
  # Preserve whitespace, option order, kind, destinations and all other flags.
  rest = $0
  output = ""
  while (match(rest, /[^ \t]+/)) {
    output = output substr(rest, 1, RSTART - 1)
    token = substr(rest, RSTART, RLENGTH)
    rest = substr(rest, RSTART + RLENGTH)
    if (token in canonical) {
      output = output canonical[token]
      seen[token] = 1
    } else {
      if (token ~ /^-f(file|debug|macro)-prefix-map=/)
        fail("undeclared prefix map in checksum options")
      output = output token
    }
  }
  print output rest
}

END {
  if (!failed)
    for (token in canonical)
      if (!(token in seen))
        fail("declared prefix map is absent from checksum options")
}
