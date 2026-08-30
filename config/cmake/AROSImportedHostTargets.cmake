# LLVM's installed CMake exports describe its host-side shared libraries as
# IMPORTED SHARED targets. Cross runtime builds only consume those imports;
# every AROS output in these builds remains explicitly static.
#
# CMake 3.30's CMP0164 rejects even imported SHARED targets after the Generic
# or AROS platform marks target shared libraries unsupported. This project
# include runs after project() has loaded the target platform and permits the
# host imports without changing BUILD_SHARED_LIBS or any AROS link rule.
set_property(GLOBAL PROPERTY TARGET_SUPPORTS_SHARED_LIBS TRUE)
