cmake_minimum_required(VERSION 3.24)
project(surface_ir_hello VERSION 0.2.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
option(SURFACE_IR_KDE "Build the KDE System Settings enrollment page" OFF)
if(SURFACE_IR_KDE)
    find_package(ECM 6.0 REQUIRED NO_MODULE)
    set(CMAKE_MODULE_PATH ${ECM_MODULE_PATH})
    include(KDEInstallDirs)
    include(KDECMakeSettings)
else()
    include(GNUInstallDirs)
endif()

set(CMAKE_AUTOMOC OFF)
set(CMAKE_AUTORCC OFF)

find_package(OpenCV REQUIRED COMPONENTS core imgproc imgcodecs objdetect)
find_package(OpenSSL REQUIRED COMPONENTS Crypto)
find_package(CURL 7.85 REQUIRED)
find_package(PkgConfig REQUIRED)
find_package(Threads REQUIRED)
find_package(tomlplusplus CONFIG REQUIRED)
pkg_check_modules(JsonCpp REQUIRED IMPORTED_TARGET jsoncpp)
find_library(PAM_LIBRARY pam REQUIRED)

add_library(surface_ir_core STATIC
    src/native/camera.cpp
    src/native/data.cpp
    src/native/enrollment.cpp
    src/native/protocol.cpp
    src/native/recognition.cpp
)
target_link_libraries(surface_ir_core PUBLIC ${OpenCV_LIBS} PkgConfig::JsonCpp
    PRIVATE Threads::Threads CURL::libcurl OpenSSL::Crypto tomlplusplus::tomlplusplus)
target_include_directories(surface_ir_core PUBLIC src/native ${OpenCV_INCLUDE_DIRS})
target_compile_options(surface_ir_core PRIVATE -Wall -Wextra -Wpedantic)

add_executable(surface-ir src/native/cli.cpp)
target_link_libraries(surface-ir PRIVATE surface_ir_core)
target_compile_definitions(surface-ir PRIVATE SURFACE_IR_VERSION="${PROJECT_VERSION}")

add_executable(surface-ir-hellod src/native/daemon.cpp)
target_link_libraries(surface-ir-hellod PRIVATE surface_ir_core)

add_library(pam_surface_ir MODULE pam/pam_surface_ir.cpp)
set_target_properties(pam_surface_ir PROPERTIES PREFIX "")
target_include_directories(pam_surface_ir PRIVATE src/native)
target_link_libraries(pam_surface_ir PRIVATE ${PAM_LIBRARY})

configure_file(packaging/surface-ir-hellod.service.in surface-ir-hellod.service @ONLY)
install(TARGETS surface-ir surface-ir-hellod RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR} COMPONENT Core)
install(TARGETS pam_surface_ir LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}/security COMPONENT Core)
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/surface-ir-hellod.service
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/systemd/system COMPONENT Core)
install(FILES LICENSE DESTINATION ${CMAKE_INSTALL_DATADIR}/licenses/surface-ir-hello COMPONENT Core)
install(FILES packaging/kde-face.pam DESTINATION ${CMAKE_INSTALL_DATADIR}/surface-ir-hello COMPONENT Core)
install(CODE "
    if(NOT EXISTS \"\$ENV{DESTDIR}/etc/surface-ir-hello/config.toml\")
        file(INSTALL DESTINATION \"/etc/surface-ir-hello\" TYPE FILE
            FILES \"${CMAKE_CURRENT_SOURCE_DIR}/packaging/config.toml\")
    endif()
" COMPONENT Core)

if(SURFACE_IR_KDE)
    add_subdirectory(kcm)
endif()

include(CTest)
if(BUILD_TESTING)
    add_executable(surface-ir-regression tests/native_regression.cpp)
    target_link_libraries(surface-ir-regression PRIVATE surface_ir_core)
    add_test(NAME native-regression COMMAND surface-ir-regression)
endif()
