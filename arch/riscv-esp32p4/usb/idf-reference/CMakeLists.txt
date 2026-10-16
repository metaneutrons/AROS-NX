cmake_minimum_required(VERSION 3.22)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
idf_build_set_property(MINIMAL_BUILD ON)
project(p4_hs_reference)
if(HS_REFERENCE_FORCE_FS_HSPHY)
    include("${CMAKE_CURRENT_LIST_DIR}/force_fs_hsphy.cmake")
    idf_component_get_property(hs_tinyusb_target espressif__tinyusb COMPONENT_LIB)
    idf_component_get_property(hs_tinyusb_dir espressif__tinyusb COMPONENT_DIR)
    hs_reference_force_fs_hsphy("${hs_tinyusb_target}"
        "${hs_tinyusb_dir}/src/portable/synopsys/dwc2/dcd_dwc2.c"
        "${CMAKE_BINARY_DIR}/hs-fs-comparison/dcd_dwc2.c")
endif()
if(HS_REFERENCE_RX_TRACE)
    include("${CMAKE_CURRENT_LIST_DIR}/force_fs_hsphy.cmake")
    idf_component_get_property(hs_tinyusb_target espressif__tinyusb COMPONENT_LIB)
    idf_component_get_property(hs_tinyusb_dir espressif__tinyusb COMPONENT_DIR)
    hs_reference_force_fs_hsphy("${hs_tinyusb_target}"
        "${hs_tinyusb_dir}/src/portable/synopsys/dwc2/dcd_dwc2.c"
        "${CMAKE_BINARY_DIR}/hs-rx-trace/dcd_dwc2.c" RX_TRACE)
    target_include_directories(${hs_tinyusb_target} PRIVATE "${CMAKE_CURRENT_LIST_DIR}/main")
endif()
