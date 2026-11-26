set(CMAKE_INSTALL_DEFAULT_COMPONENT_NAME KDE)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
find_package(Qt6 6.5 REQUIRED COMPONENTS Core Quick)
find_package(KF6 6.0 REQUIRED COMPONENTS CoreAddons I18n KCMUtils)

set(helper_dir ${KDE_INSTALL_LIBDIR}/libexec/surface-ir-hello)
set(SURFACE_IR_HELPER_PATH ${KDE_INSTALL_FULL_LIBDIR}/libexec/surface-ir-hello/surface-ir-enroll-helper)
add_executable(surface-ir-enroll-helper ../src/native/enroll_helper.cpp)
target_link_libraries(surface-ir-enroll-helper PRIVATE surface_ir_core OpenSSL::Crypto)
install(TARGETS surface-ir-enroll-helper RUNTIME DESTINATION ${helper_dir} COMPONENT KDE)

kcmutils_add_qml_kcm(kcm_surfaceir)
target_sources(kcm_surfaceir PRIVATE surfaceirsettings.cpp)
target_link_libraries(kcm_surfaceir PRIVATE Qt6::Quick KF6::CoreAddons KF6::I18n KF6::KCMUtilsQuick)
target_compile_definitions(kcm_surfaceir PRIVATE SURFACE_IR_HELPER_PATH="${SURFACE_IR_HELPER_PATH}")

configure_file(org.surfaceirhello.enroll.policy.in org.surfaceirhello.enroll.policy @ONLY)
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/org.surfaceirhello.enroll.policy
    DESTINATION ${KDE_INSTALL_DATADIR}/polkit-1/actions COMPONENT KDE)
install(FILES ../LICENSE DESTINATION ${CMAKE_INSTALL_DATADIR}/licenses/surface-ir-hello-kde COMPONENT KDE)
