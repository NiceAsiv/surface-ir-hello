cmake_minimum_required(VERSION 3.24)
project(surface_ir_settings LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
find_package(ECM 6.0 REQUIRED NO_MODULE)
set(CMAKE_MODULE_PATH ${ECM_MODULE_PATH})
include(KDEInstallDirs)
include(KDECMakeSettings)

find_package(Qt6 6.5 REQUIRED COMPONENTS Core Quick)
find_package(KF6 6.0 REQUIRED COMPONENTS CoreAddons I18n KCMUtils)

set(SURFACE_IR_HELPER_PATH ${CMAKE_INSTALL_FULL_LIBEXECDIR}/surface-ir-hello/surface-ir-enroll-helper)
kcmutils_add_qml_kcm(kcm_surfaceir)
target_sources(kcm_surfaceir PRIVATE surfaceirsettings.cpp)
target_link_libraries(kcm_surfaceir PRIVATE Qt6::Quick KF6::CoreAddons KF6::I18n KF6::KCMUtilsQuick)
target_compile_definitions(kcm_surfaceir PRIVATE SURFACE_IR_HELPER_PATH="${SURFACE_IR_HELPER_PATH}")

configure_file(org.surfaceirhello.enroll.policy.in org.surfaceirhello.enroll.policy @ONLY)
install(PROGRAMS surface-ir-enroll-helper DESTINATION ${KDE_INSTALL_LIBEXECDIR}/surface-ir-hello)
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/org.surfaceirhello.enroll.policy DESTINATION ${KDE_INSTALL_DATADIR}/polkit-1/actions)
