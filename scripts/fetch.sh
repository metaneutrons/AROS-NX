#!/usr/bin/env bash

# Copyright @ 2004-2026, The AROS Development Team. All rights reserved.
# $Id$

SPOOFED_USER_AGENT="Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/125.0.0.0 Safari/537.36"

ALWAYSUNPACK="no"
LOCATION="./"
BASE="./"

error()
{    
    echo "$1"
    exit 1
}

usage()
{
    error "Usage: $1 -a archive [-s suffixes] [-ao archive_origins...] [-cs filename=sha256:digest...] [-l location to download to] [-d dir to unpack to] [-po patches_origins...] [-p patch[:subdir][:patch options]...]"
}

is_enabled()
{
    case "$1" in
        1|yes|true|on|YES|TRUE|ON) return 0 ;;
        *) return 1 ;;
    esac
}

is_remote_origin()
{
    case "$1" in
        http://*|https://*|ftp://*|archives://*|gnu://*|sf://*|sourceforge://*|github://*|cache://*)
            return 0 ;;
        *)
            return 1 ;;
    esac
}

checksum_for()
{
    local wanted="$1" entry name value found=""
    for entry in $checksums; do
        name=${entry%%=*}
        value=${entry#*=}
        if test "$name" = "$wanted"; then
            if test -n "$found"; then
                echo "fetch: duplicate checksum declaration for '$wanted'." >&2
                return 2
            fi
            found="$value"
        fi
    done
    test -n "$found" || return 1
    printf '%s\n' "$found"
}

validate_checksum_contract()
{
    local entry name value candidate candidate_known

    for entry in $checksums; do
        case "$entry" in
            *=sha256:*) ;;
            *)
                echo "fetch: invalid checksum '$entry'; expected filename=sha256:<64 hex digits>." >&2
                return 1 ;;
        esac
        name=${entry%%=*}
        value=${entry#*=sha256:}
        case "$name" in
            ""|*/*|*\\*|*[!A-Za-z0-9._+~-]*)
                echo "fetch: invalid checksum filename '$name'; use an exact archive basename." >&2
                return 1 ;;
        esac
        if test ${#value} -ne 64 || test -n "$(printf '%s' "$value" | tr -d '0123456789abcdefABCDEF')"; then
            echo "fetch: invalid SHA-256 for '$name'; expected exactly 64 hexadecimal digits." >&2
            return 1
        fi
        candidate_known=no
        if test -n "$suffixes"; then
            for candidate in $suffixes; do
                test "$name" = "$archive.$candidate" && candidate_known=yes
            done
        elif test "$name" = "$archive"; then
            candidate_known=yes
        fi
        if test "$candidate_known" != yes; then
            echo "fetch: checksum filename '$name' is not a declared candidate for archive '$archive'." >&2
            return 1
        fi
        checksum_for "$name" >/dev/null || return 1
    done

    if is_enabled "${AROS_FETCH_REQUIRE_CHECKSUMS:-0}" && test -z "$checksums"; then
        echo "fetch: strict checksum mode requires an explicit checksum for archive '$archive'." >&2
        echo "fetch: add checksums=\"<filename>=sha256:<digest>\" to its %fetch declaration." >&2
        return 1
    fi
    test -z "$checksums" && return 0

    if test -n "$suffixes"; then
        for candidate in $suffixes; do
            if ! checksum_for "$archive.$candidate" >/dev/null; then
                echo "fetch: checksum contract for '$archive' does not cover candidate '$archive.$candidate'." >&2
                echo "fetch: every suffix candidate must be explicit; no unverified fallback is allowed." >&2
                return 1
            fi
        done
    elif ! checksum_for "$archive" >/dev/null; then
        echo "fetch: checksum contract for '$archive' does not contain '$archive'." >&2
        return 1
    fi
}

sha256_file()
{
    local path="$1"
    if command -v sha256sum >/dev/null 2>&1; then
        sha256sum "$path" | awk '{print $1}'
    elif command -v shasum >/dev/null 2>&1; then
        shasum -a 256 "$path" | awk '{print $1}'
    elif command -v openssl >/dev/null 2>&1; then
        openssl dgst -sha256 "$path" | awk '{print $NF}'
    else
        echo "fetch: cannot verify SHA-256; install sha256sum, shasum, or openssl." >&2
        return 1
    fi
}

verify_checksum()
{
    local file="$1" expected_entry expected actual
    expected_entry=$(checksum_for "$file") || return $?
    expected=${expected_entry#sha256:}
    actual=$(sha256_file "$LOCATION/$file") || return 1
    expected=$(printf '%s' "$expected" | tr 'A-F' 'a-f')
    actual=$(printf '%s' "$actual" | tr 'A-F' 'a-f')
    if test "$actual" != "$expected"; then
        echo "fetch: SHA-256 mismatch for '$file'." >&2
        echo "fetch: expected: $expected" >&2
        echo "fetch: actual:   $actual" >&2
        echo "fetch: cached file: $LOCATION/$file" >&2
        echo "fetch: remove or replace the file only after verifying the declared upstream payload." >&2
        return 1
    fi
    echo "Verified   $file (SHA-256)"
}

fetch_mirrored()
{
    local origin="$1" file="$2" destination="$3" mirrosgroup="$4" mirrors="$5"
    local ret=false

    for mirror in $mirrors; do
        echo "Downloading from ${mirrosgroup}... "
        if fetch "${mirror}/$origin" "${file}" "$destination"; then
                ret=true
                break;
        fi
    done
    
    $ret
}

cache_base="https://github.com/aros-development-team/external-sources/raw/refs/heads/main"

fetch_ghcache()
{
    local origin="$1" file="$2" destination="$3"
    local ret=true

    # `origin` here is the path after "cache://"
    # Normalize away any leading slash
    local rel="${origin#/}"

    # Build a new HTTPS origin and reuse `fetch()` for actual HTTP handling
    local new_origin="$cache_base"
    if [ -n "$rel" ]; then
        new_origin="$cache_base/$rel"
    fi

    if ! fetch "$new_origin" "$file" "$destination"; then
        ret=false
    fi

    $ret
}

gnu_mirrors="http://mirror.netcologne.de/gnu http://ftpmirror.gnu.org http://ftp.gnu.org/pub/gnu"

fetch_gnu()
{
    local origin="$1" file="$2" destination="$3"
    local ret=true

    if ! fetch_mirrored "$origin" "${file}" "$destination" "GNU" "${gnu_mirrors}"; then
        ret=false
    fi

    $ret
}

archives_sources="http://archives.aros-exec.org https://arosarchives.os4depot.net"

fetch_archives()
{
    local origin="$1" file="$2" destination="$3"
    local ret=true

    if ! fetch_mirrored "$origin" "${file}" "$destination" "Archives" "${archives_sources}"; then
        ret=false
    fi

    $ret
}

# Note: at time of writing, the main SF download server redirects to mirrors
# and corrects moved paths, so we retry this server multiple times (in one
# wget call).
sf_mirrors="http://downloads.sourceforge.net"

fetch_sf()
{
    local origin="$1" file="$2" destination="$3"
    local ret=true

    if ! fetch_mirrored "$origin" "${file}" "$destination" "SourceForge" "${sf_mirrors}"; then
        ret=false
    fi

    $ret
}

github_mirrors="https://github.com"

fetch_github()
{
    local origin="$1" file="$2" destination="$3"
    local ret=true

    if ! fetch_mirrored "$origin" "$file" "$destination" "Github" "${github_mirrors}"; then
        ret=false
    fi

    $ret
}

curl_ftp()
{
    local curlsrc="$1" curloutput="$2"
    local curlextraflags
    local ret=true

    for (( ; ; ))
    do
        if !  eval "curl -f --retry 3 --retry-connrefused $curlextraflags --speed-limit 1 --speed-time 15 -C - \"$curlsrc\" -o $curloutput"; then
            if test "$ret" = false; then
                break
            fi
            ret=false
            curlextraflags="--tlsv1 --tls-max 1.0"
        else
            ret=true
            break
        fi
    done

    $ret
}

curl_http() {
    local tryurl="$1" curloutput="$2"
    local curlextraflags=""
    local curlext=""
    local curlsrc
    local urlsrc
    local ret
    local state=0

    local protocol

    if echo "$tryurl" | grep "prdownloads.sourceforge.net" >/dev/null; then
        if ! echo "$tryurl" | grep "/download" >/dev/null; then
            curlext="/download"
        fi
    fi

    protocol=$(echo "$tryurl$curlext" | cut -d':' -f1)

    urlsrc=$(curl -fsIL -o /dev/null -w "%{url_effective}" "$tryurl$curlext")
    if [ $? -ne 0 ]; then
        # In case an old confused server is encountered we assume TLS 1.0.
        # The system this script runs on might not support TLS 1.0,
        # and then we are out of luck, but we tried.
        # Servers of this kind should be super rare, but the fallback was
        # carried over from the original code using wget.
        urlsrc=$(curl --tlsv1 --tls-max 1.0 -fsIL -o /dev/null -w "%{url_effective}" "$tryurl$curlext")
    fi
    if [ $? -ne 0 ]; then
        curlsrc="$tryurl"
    else
        curlsrc="$urlsrc"
    fi

    while :; do
        eval "curl -fL --retry 3 --retry-connrefused $curlextraflags --speed-limit 1 --speed-time 15 -C - \"$curlsrc\" -o \"$curloutput\""
        ret=$?

        if [ $ret -eq 0 ]; then
            return 0
        fi

        # In case the requested URL was HTTP, but has been redirected to HTTPS with
        # SSL certificate failure (curl exit code 60) just use insecure mode
        # (we didnt care in the first place, since we use HTTP)
        if [ $ret -eq 60 ] && [ "$protocol" = "http" ] && [ $state -lt 3 ]; then
            curlextraflags="$curlextraflags -k"
            state=3
            continue
        fi

        case $state in
            0)
                # First failure, try with older TLS
                curlextraflags="--tlsv1 --tls-max 1.1"
                state=1
                ;;
            1)
                # Second failure, spoof user agent and referer
                if ! echo "$tryurl" | grep "prdownloads.sourceforge.net" >/dev/null; then
                    curlextraflags="--tlsv1.2 --tls-max 1.2 -A \"$SPOOFED_USER_AGENT\" -e \"$curlsrc\""
                    state=2
                else
                    return 1
                fi
                ;;
            *)
                # Final failure, exit
                return 1
                ;;
        esac
    done
}

fetch()
{
    local origin="$1" file="$2" destination="$3"
    
    local protocol

    if echo "$origin" | grep ":" >/dev/null; then
        protocol=$(echo "$origin" | cut -d':' -f1)
    fi

    local ret=true
    
    trap 'rm -f "$destination/$file".tmp; exit' SIGINT SIGKILL SIGTERM

    case $protocol in
        https| http)
            if ! curl_http "$origin/$file" "$destination/$file.tmp"; then
                ret=false
            else
                mv "$destination/$file.tmp" "$destination/$file"
            fi
            rm -f "$destination/$file.tmp"
            ;;
        ftp)    
            if ! curl_ftp "$origin/$file" "$destination/$file.tmp"; then
                ret=false
            else
                mv "$destination/$file.tmp" "$destination/$file"
            fi
            rm -f "$destination/$file.tmp"
            ;;
        archives)
            if ! fetch_archives "${origin:${#protocol}+3}" "$file" "$destination"; then
                ret=false
            fi
            ;;
        gnu)
            if ! fetch_gnu "${origin:${#protocol}+3}" "$file" "$destination"; then
                ret=false
            fi
            ;;
        sf | sourceforge)
            if ! fetch_sf "${origin:${#protocol}+3}" "$file" "$destination"; then
                ret=false
            fi
            ;;
        github)
            if ! fetch_github "${origin:${#protocol}+3}" "$file" "$destination"; then
                ret=false
            fi
            ;;
        cache)
            if ! fetch_ghcache "${origin:${#protocol}+3}" "$file" "$destination"; then
                ret=false
            fi
            ;;
	*)
	    if test "$origin" = "$destination";  then
	        ! test -f "$origin/$file" && ret=false
	    else
	        if ! cp "$origin/$file" "$destination/$file.tmp" >/dev/null; then
		    ret=false
		else
		    mv "$destination/$file.tmp" "$destination/$file"
		fi
	    fi
	    ;;
    esac
    
    trap SIGINT SIGKILL SIGTERM
    
    $ret
}

fetch_multiple()
{
    local origins="$1" file="$2" destination="$3"
    for origin in $origins; do
        if is_enabled "${AROS_FETCH_OFFLINE:-0}" && is_remote_origin "$origin"; then
            echo "Offline     skipping network origin $origin/$file"
            continue
        fi
        echo "Trying     $origin/$file..."
        fetch "$origin" "$file" "$destination" && return  0
    done

    return 1
}

fetch_cached()
{ 
    local origins="$1" file="$2" suffixes="$3" destination="$4" foundvar="$5"
    
    local __dummy__
    foundvar=${foundvar:-__dummy__}
    
    export "$foundvar"=
    
    test -e "$destination" -a ! -d "$destination" && \
        echo "fetch_cached: \`$destination' is not a diretory." && return 1

    if ! test -e "$destination"; then
	echo "fetch_cached: \`$destination' does not exist. Making it."
	! mkdir -p "$destination" && return 1
    fi
    
    if test "x$suffixes" != "x"; then
        for sfx in $suffixes; do
	    fetch_multiple "$destination" "$file.$sfx" "$destination" && \
	        export "$foundvar"="$file.$sfx" && return 0
        done
       
	for sfx in $suffixes; do
	    fetch_multiple "$origins" "$file.$sfx" "$destination" && \
	        export "$foundvar"="$file.$sfx" && return 0
        done    
    else
        fetch_multiple "$destination" "$file" "$destination" && \
	    export "$foundvar"="$file" && return 0
        fetch_multiple "$origins" "$file" "$destination" && \
	    export "$foundvar"="$file" && return 0
    fi
    
    return 1
}

unpack()
{
    local location="$1" archive="$2" archivepath="$3";
    
    local old_PWD="$PWD"
    cd "$location"
    
    echo "Unpacking  \`$archive'..."
    
    local ret=true
    case "$archive" in
        *.tar.bz2)
	    if ! tar xfj "$archivepath/$archive"; then ret=false; fi
	    ;;
        *.tar.gz | *.tgz)
	    if ! tar xfz "$archivepath/$archive"; then ret=false; fi
	    ;;
        *.zip)
	    if ! unzip "$archivepath/$archive"; then ret=false; fi
	    ;;
        *.tar.xz)
	    if ! tar xfJ "$archivepath/$archive"; then ret=false; fi
	    ;;
	*)
	    echo "Unknown archive format for \`$archive'."
	    ret=false
	    ;;
    esac
	
    cd "$old_PWD"
    
    $ret
}

unpack_cached()
{
    local location="$1" archive="$2" archivepath="$3";

    if ! test -e "$location"; then
	echo "unpack_cached: \`$location' does not exist. Making it."
	! mkdir -p "$location" && return 1
    fi

    if ! test "$ALWAYSUNPACK" = "no" -a -f "$BASE/.${archive}.unpacked"; then
        if unpack "$location" "$archive" "$archivepath"; then
	    echo yes > "$BASE/.${archive}.unpacked"
	    true
	else
	    false
	fi
    fi
}

do_patch()
{
    local location="$1" patch_spec="$2";
    
    local old_PWD="$PWD"
    cd "$location"
    
    local patch=$(echo "$patch_spec": | cut -d: -f1)
    local subdir=$(echo "$patch_spec": | cut -d: -f2)
    local patch_opt=$(echo "$patch_spec": | cut -d: -f3 | sed -e "s/,/ /g")
    local patch_cmd="patch -Z -E $patch_opt < $BASE/$patch"
        
    cd "${subdir:-.}"  2> /dev/null;
    
    local ret=true

    if ! eval "$patch_cmd" ; then
        ret=false
    fi

    cd "$old_PWD"
    
    $ret
}

patch_cached()
{
    local location="$1" patch="$2";
    
    local patchname=$(echo "$patch" | cut -d: -f1)
    
    if test "x$patchname" != "x"; then
        if ! test -f "$BASE/.${patchname}.applied";  then
            if do_patch "$location" "$patch"; then
	        echo yes > "$BASE/.${patchname}.applied"
	        true
	    else
	        false
	    fi
        fi
    fi
}

fetchlock()
{
    local location="$1" archive="$2" localbuild="$3" ;
    local notified=0;

    for (( ; ; ))
    do
        if ! test -f "$location"; then
            local lockfile="${location}/${archive}.fetch"
            if ! test -f "$lockfile"; then
                trap "fetchunlock $location $archive" INT
                echo "$$" > "$lockfile"
                break
            else
                local pid=$(tr -d '\0' < "$lockfile")
                case "$pid" in
                    ""|*[!0-9]*)
                        # No valid PID recorded (empty or corrupt lockfile):
                        # waiting on it can never succeed, so reclaim the lock.
                        echo "fetch.sh: removing corrupt lockfile (no valid PID): $lockfile"
                        fetchunlock "$location" "$archive"
                        continue
                        ;;
                esac
                if [ -n "$localbuild" ]; then
                    if ! kill -0 "$pid" 2>/dev/null; then
                        echo "fetch.sh: removing stale lockfile (no process with PID: $pid)"
                        fetchunlock "$location" "$archive"
                        continue
                    fi
                fi

                if test "$notified" -ne 1; then
                    echo "fetch.sh: waiting for process with PID: $pid to finish downloading..."
                    notified=1
                fi
                sleep 1
            fi
        else
            break
        fi
    done
}

fetchunlock()
{
    local location="$1" archive="$2";

    if test -f "${location}/${archive}.fetch"; then
        rm -f "${location}/${archive}.fetch"
    fi
}


while test "x$1" != "x"; do
    case "$1" in
        -ao|-a|-s|-d|-po|-p|-b|-l|-rn|-cs)
            # Ensure there *is* a $2 (even if it's an empty string)
            if test $# -lt 2; then
                echo "Option $1 requires an argument."
                usage "$0"
            fi
            # If $2 is non-empty and starts with '-', treat as missing
            if test "x${2}" != "x" && test "x${2:0:1}" = "x-"; then
                echo "Option $1 requires an argument."
                usage "$0"
            fi
            case "$1" in
                -ao) archive_origins="$2";;
                -a)  archive="$2";;
                -s)  suffixes="$2";;
                -d)  destination="$2";;
                -po) patches_origins="$2";;
                -p)  patches="$2";;
                -b)  newbase="$2";;
                -l)  newlocation="$2";;
                -rn) renamedir="$2";;
                -cs) checksums="$2";;
            esac
            shift 2;;
        -f)
            ALWAYSUNPACK="yes"; shift 1;;
        *)
            echo "fetch: Unknown option \`$1'."
            usage "$0";;
    esac
done

test -z "$archive" && usage "$0"

archive_origins=${archive_origins:-.}
destination=${destination:-.}
LOCATION=${newlocation:-.}
BASE=${newbase:-${destination}}
patches_origins=${patches_origins:-.}

validate_checksum_contract || exit 1

# Ensure LOCATION, destination, and BASE are absolute paths.
[[ "$LOCATION" != /* ]] && LOCATION="$PWD/$LOCATION"
[[ "$destination" != /* ]] && destination="$PWD/$destination"
[[ "$BASE" != /* ]] && BASE="$PWD/$BASE"

fetchlockfile="$archive"
fetchlock "$LOCATION" "$fetchlockfile"

fetch_cached "$archive_origins" "$archive" "$suffixes" "$LOCATION" archive2

if test -z "$archive2"; then
    fetchunlock "$LOCATION" "$fetchlockfile"
    if is_enabled "${AROS_FETCH_OFFLINE:-0}"; then
        echo "fetch: offline cache/local-origin miss for archive '$archive' in '$LOCATION'." >&2
        echo "fetch: seed this cache with the declared archive, or rerun without offline mode." >&2
    fi
    error "fetch: Error while fetching the archive \`$archive'."
fi
archive="$archive2"

if test -n "$checksums" || is_enabled "${AROS_FETCH_REQUIRE_CHECKSUMS:-0}"; then
    if ! verify_checksum "$archive"; then
        fetchunlock "$LOCATION" "$fetchlockfile"
        error "fetch: refusing to unpack unverified archive \`$archive'."
    fi
fi

for patch in $patches; do
    patch=$(echo "$patch" | cut -d: -f1)
    if test "x$patch" != "x"; then
        if ! fetch_cached "$patches_origins" "$patch" "" "$BASE"; then
            fetch_cached "$patches_origins" "$patch" "tar.bz2 tar.gz zip" "$BASE" patch2
            test -z "$patch2" && error "fetch: Error while fetching the patch \`$patch'."
            if ! unpack_cached "$destination" "$patch2" "$destination"; then
                fetchunlock "$LOCATION" "$fetchlockfile"
                error "fetch: Error while unpacking \`$patch2'."
            fi
        fi
    fi
done

if test "x$suffixes" != "x"; then
    if ! unpack_cached "$destination" "$archive" "$LOCATION"; then
        fetchunlock "$LOCATION" "$fetchlockfile"
        error "fetch: Error while unpacking \`$archive'."
    fi
fi
    
for patch in $patches; do
    if ! patch_cached "$destination" "$patch"; then
        fetchunlock "$LOCATION" "$fetchlockfile"
        error "fetch: Error while applying the patch \`$patch'."
    fi
done

fetchunlock "$LOCATION" "$fetchlockfile"
