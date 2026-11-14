cmake_minimum_required(VERSION 3.16)

# Qt is located through CMAKE_PREFIX_PATH (cache variable or environment).
find_package(Qt6 REQUIRED COMPONENTS Core DBus Gui Network Qml Quick QuickControls2 Test)

qt_policy(SET QTP0001 NEW)

set(CMAKE_AUTOMOC ON)

add_library(isvn_gui_backend STATIC
    src/backend/IsvnCli.cpp
    src/backend/GuiInstanceCoordinator.cpp
    src/backend/LaunchPaths.cpp
    src/backend/SafeNotifier.cpp
    src/backend/RoleResolver.cpp
    src/backend/WorkSessionStore.cpp
)
target_include_directories(isvn_gui_backend PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(isvn_gui_backend PUBLIC Qt6::Core Qt6::DBus Qt6::Network)
target_compile_options(isvn_gui_backend PRIVATE -Wall -Wextra -Wpedantic)

add_executable(isvn-gui
    src/main.cpp
)
target_link_libraries(isvn-gui PRIVATE
    isvn_gui_backend
    isvn_version
    Qt6::Core
    Qt6::Gui
    Qt6::Qml
    Qt6::Quick
    Qt6::QuickControls2
)
target_compile_options(isvn-gui PRIVATE -Wall -Wextra -Wpedantic)

qt_add_qml_module(isvn-gui
    URI Isvn.Gui
    VERSION 1.0
    QML_FILES
        qml/Main.qml
        qml/common/VersionNode.qml
        qml/common/FileStatusRow.qml
        qml/common/MetadataPopover.qml
        qml/common/ConfirmDialog.qml
        qml/common/CommandRunner.qml
        qml/common/BusyIndicator.qml
        qml/common/I18n.qml
        qml/common/Theme.qml
        qml/common/InfoTip.qml
        qml/common/ShellPanel.qml
        qml/common/StatusDot.qml
        qml/common/RoleBadge.qml
        qml/common/ActionButton.qml
        qml/common/EmptyState.qml
        qml/common/SectionHeader.qml
        qml/common/InspectorSection.qml
        qml/common/KeyValueGrid.qml
        qml/common/VersionPathPill.qml
        qml/common/HashPill.qml
        qml/common/StageLegend.qml
        qml/common/GraphToolbar.qml
        qml/common/GraphNodeCard.qml
        qml/common/FileStateBadge.qml
        qml/common/SeverityBanner.qml
        qml/common/ComponentGallery.qml
        qml/workspaceA/WorkspaceA.qml
        qml/workspaceA/ProjectVersionTree.qml
        qml/workspaceB/WorkspaceB.qml
        qml/workspaceC/WorkspaceC.qml
        # --- Product Shell: ProductShell is the default shell; the complete Main.qml
        # stays available as the ISVN_GUI_SHELL=legacy runtime rollback. ---
        qml/shell/ProductShell.qml
        qml/shell/ProductShellWindow.qml
        qml/shell/AccessDeniedWindow.qml
        qml/shell/HomePage.qml
        qml/shell/ExplorePage.qml
        qml/shell/ExploreCheckoutController.qml
        qml/shell/MaintenancePage.qml
        qml/shell/ProductNavigation.qml
        qml/shell/TopCommandBar.qml
        qml/shell/BottomStatusBar.qml
        qml/work/WorkPage.qml
        qml/work/WorkHeader.qml
        qml/work/LocalVersionNavigator.qml
        qml/work/FileStatusWorkspace.qml
        qml/work/WorkflowPanel.qml
        qml/work/CheckinBatchOverlay.qml
        qml/work/ImportController.qml
        qml/work/ImportOverlay.qml
        qml/work/CheckinReview.qml
        qml/work/CheckinConfirm.qml
        qml/work/CheckinResult.qml
        qml/work/OutcomeUnknown.qml
        qml/work/ImportPreview.qml
        qml/work/OperationStepper.qml
        qml/work/WorkMockModel.qml
        qml/work/WorkUiState.qml
        qml/work/WorkReadController.qml
        qml/work/WorkActionController.qml
        qml/work/CheckinBatchController.qml
        qml/work/OperationCoordinator.qml
        qml/work/WorkOperationOverlay.qml
)

# Install layout: the GUI binary lives in libexec/ and is started through the
# isvn-gui launcher, which loads Qt from the install's "qt" symlink.  The binary
# carries only a relative RUNPATH; the Qt prefix is chosen at install time and
# must be the same Qt version the GUI was built with.
set_target_properties(isvn-gui PROPERTIES INSTALL_RPATH "\$ORIGIN/../qt/lib")
install(TARGETS isvn-gui RUNTIME DESTINATION libexec)
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/isvn-gui.qt-version" "${Qt6_VERSION}\n")
install(FILES "${CMAKE_CURRENT_BINARY_DIR}/isvn-gui.qt-version" DESTINATION libexec)
install(PROGRAMS packaging/isvn-gui.sh DESTINATION . RENAME isvn-gui)

if(BUILD_TESTING)
    add_executable(test_gui_backend
        tests/test_gui_backend.cpp
    )
    target_link_libraries(test_gui_backend PRIVATE isvn_gui_backend Qt6::Core Qt6::Test)
    target_compile_options(test_gui_backend PRIVATE -Wall -Wextra -Wpedantic)
    add_test(NAME gui_backend COMMAND test_gui_backend)

    add_executable(test_gui_instance_coordinator
        tests/test_gui_instance_coordinator.cpp
    )
    target_link_libraries(test_gui_instance_coordinator PRIVATE
        isvn_gui_backend Qt6::Core Qt6::Network Qt6::Test
    )
    target_compile_options(test_gui_instance_coordinator PRIVATE -Wall -Wextra -Wpedantic)
    add_test(NAME gui_instance_coordinator COMMAND test_gui_instance_coordinator)

    add_executable(test_product_shell_clicks
        tests/test_product_shell_clicks.cpp
    )
    target_link_libraries(test_product_shell_clicks PRIVATE
        Qt6::Core Qt6::Gui Qt6::Qml Qt6::Quick Qt6::QuickControls2 Qt6::Test
    )
    target_compile_definitions(test_product_shell_clicks PRIVATE
        ISVN_GUI_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}"
    )
    target_compile_options(test_product_shell_clicks PRIVATE -Wall -Wextra -Wpedantic)
    add_test(NAME gui_product_shell_clicks COMMAND test_product_shell_clicks)
    set_tests_properties(gui_product_shell_clicks PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

    add_executable(test_product_shell_work_binding
        tests/test_product_shell_work_binding.cpp
    )
    target_link_libraries(test_product_shell_work_binding PRIVATE
        isvn_gui_backend
        Qt6::Core Qt6::Gui Qt6::Qml Qt6::Quick Qt6::QuickControls2 Qt6::Test
    )
    target_compile_definitions(test_product_shell_work_binding PRIVATE
        ISVN_GUI_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}"
    )
    target_compile_options(test_product_shell_work_binding PRIVATE -Wall -Wextra -Wpedantic)
    add_test(NAME gui_product_shell_work_binding COMMAND test_product_shell_work_binding)
    set_tests_properties(gui_product_shell_work_binding PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

    add_executable(test_product_shell_entry
        tests/test_product_shell_entry.cpp
    )
    # The GUI starts only with its paired CLI beside it.
    add_dependencies(test_product_shell_entry isvn-gui isvn)
    target_link_libraries(test_product_shell_entry PRIVATE Qt6::Core Qt6::Test)
    target_compile_definitions(test_product_shell_entry PRIVATE
        ISVN_GUI_BINARY="$<TARGET_FILE:isvn-gui>"
        ISVN_GUI_PROJECT_DIR="${CMAKE_SOURCE_DIR}"
    )
    target_compile_options(test_product_shell_entry PRIVATE -Wall -Wextra -Wpedantic)
    add_test(NAME gui_product_shell_entry COMMAND test_product_shell_entry)

    add_executable(test_launch_paths
        tests/test_launch_paths.cpp
    )
    target_link_libraries(test_launch_paths PRIVATE isvn_gui_backend Qt6::Core Qt6::Test)
    target_compile_options(test_launch_paths PRIVATE -Wall -Wextra -Wpedantic)
    add_test(NAME gui_launch_paths COMMAND test_launch_paths)

    add_executable(test_explore_checkout
        tests/test_explore_checkout.cpp
    )
    target_link_libraries(test_explore_checkout PRIVATE
        Qt6::Core
        Qt6::Gui
        Qt6::Qml
        Qt6::Quick
        Qt6::QuickControls2
        Qt6::Test
    )
    target_compile_definitions(test_explore_checkout PRIVATE
        ISVN_GUI_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}"
    )
    target_compile_options(test_explore_checkout PRIVATE -Wall -Wextra -Wpedantic)
    add_test(NAME gui_explore_checkout COMMAND test_explore_checkout)
    set_tests_properties(gui_explore_checkout PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

    add_executable(test_work_session_store
        tests/test_work_session_store.cpp
    )
    target_link_libraries(test_work_session_store PRIVATE isvn_gui_backend Qt6::Core Qt6::Test)
    target_compile_options(test_work_session_store PRIVATE -Wall -Wextra -Wpedantic)
    add_test(NAME gui_work_session_store COMMAND test_work_session_store)

    add_executable(test_workspace_a_guardrails
        tests/test_workspace_a_guardrails.cpp
    )
    target_link_libraries(test_workspace_a_guardrails PRIVATE Qt6::Core Qt6::Test)
    target_compile_definitions(test_workspace_a_guardrails PRIVATE
        ISVN_GUI_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}"
    )
    target_compile_options(test_workspace_a_guardrails PRIVATE -Wall -Wextra -Wpedantic)
    add_test(NAME gui_workspace_a_guardrails COMMAND test_workspace_a_guardrails)

    add_executable(test_workspace_b_guardrails
        tests/test_workspace_b_guardrails.cpp
    )
    target_link_libraries(test_workspace_b_guardrails PRIVATE Qt6::Core Qt6::Test)
    target_compile_definitions(test_workspace_b_guardrails PRIVATE
        ISVN_GUI_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}"
    )
    target_compile_options(test_workspace_b_guardrails PRIVATE -Wall -Wextra -Wpedantic)
    add_test(NAME gui_workspace_b_guardrails COMMAND test_workspace_b_guardrails)

    add_executable(test_workspace_c_guardrails
        tests/test_workspace_c_guardrails.cpp
    )
    target_link_libraries(test_workspace_c_guardrails PRIVATE Qt6::Core Qt6::Test)
    target_compile_definitions(test_workspace_c_guardrails PRIVATE
        ISVN_GUI_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}"
    )
    target_compile_options(test_workspace_c_guardrails PRIVATE -Wall -Wextra -Wpedantic)
    add_test(NAME gui_workspace_c_guardrails COMMAND test_workspace_c_guardrails)

    add_executable(test_workspace_bc_clicks
        tests/test_workspace_bc_clicks.cpp
    )
    target_link_libraries(test_workspace_bc_clicks PRIVATE
        Qt6::Core
        Qt6::Gui
        Qt6::Qml
        Qt6::Quick
        Qt6::QuickControls2
        Qt6::Test
    )
    target_compile_definitions(test_workspace_bc_clicks PRIVATE
        ISVN_GUI_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}"
    )
    target_compile_options(test_workspace_bc_clicks PRIVATE -Wall -Wextra -Wpedantic)
    add_test(NAME gui_workspace_bc_clicks COMMAND test_workspace_bc_clicks)
    set_tests_properties(gui_workspace_bc_clicks PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
    )

# --- Work shell tests + preview host ---

    add_executable(test_work_v2_guardrails
        tests/test_work_v2_guardrails.cpp
    )
    target_link_libraries(test_work_v2_guardrails PRIVATE Qt6::Core Qt6::Test)
    target_compile_definitions(test_work_v2_guardrails PRIVATE
        ISVN_GUI_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}"
    )
    target_compile_options(test_work_v2_guardrails PRIVATE -Wall -Wextra -Wpedantic)
    add_test(NAME gui_work_v2_guardrails COMMAND test_work_v2_guardrails)

    add_executable(test_work_v2_clicks
        tests/test_work_v2_clicks.cpp
    )
    target_link_libraries(test_work_v2_clicks PRIVATE
        Qt6::Core
        Qt6::Gui
        Qt6::Qml
        Qt6::Quick
        Qt6::QuickControls2
        Qt6::Test
    )
    target_compile_definitions(test_work_v2_clicks PRIVATE
        ISVN_GUI_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}"
    )
    target_compile_options(test_work_v2_clicks PRIVATE -Wall -Wextra -Wpedantic)
    add_test(NAME gui_work_v2_clicks COMMAND test_work_v2_clicks)
    set_tests_properties(gui_work_v2_clicks PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
    )

    add_executable(test_work_v2_read_clicks
        tests/test_work_v2_read_clicks.cpp
    )
    target_link_libraries(test_work_v2_read_clicks PRIVATE
        isvn_gui_backend
        Qt6::Core
        Qt6::Gui
        Qt6::Qml
        Qt6::Quick
        Qt6::QuickControls2
        Qt6::Test
    )
    target_compile_definitions(test_work_v2_read_clicks PRIVATE
        ISVN_GUI_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}"
    )
    target_compile_options(test_work_v2_read_clicks PRIVATE -Wall -Wextra -Wpedantic)
    add_test(NAME gui_work_v2_read_clicks COMMAND test_work_v2_read_clicks)
    set_tests_properties(gui_work_v2_read_clicks PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
    )

    add_executable(test_work_v2_action_clicks
        tests/test_work_v2_action_clicks.cpp
    )
    target_link_libraries(test_work_v2_action_clicks PRIVATE
        isvn_gui_backend
        Qt6::Core
        Qt6::Gui
        Qt6::Qml
        Qt6::Quick
        Qt6::QuickControls2
        Qt6::Test
    )
    target_compile_definitions(test_work_v2_action_clicks PRIVATE
        ISVN_GUI_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}"
    )
    target_compile_options(test_work_v2_action_clicks PRIVATE -Wall -Wextra -Wpedantic)
    add_test(NAME gui_work_v2_action_clicks COMMAND test_work_v2_action_clicks)
    set_tests_properties(gui_work_v2_action_clicks PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
    )

    add_executable(test_work_v2_checkin_clicks
        tests/test_work_v2_checkin_clicks.cpp
    )
    target_link_libraries(test_work_v2_checkin_clicks PRIVATE
        isvn_gui_backend
        Qt6::Core
        Qt6::Gui
        Qt6::Qml
        Qt6::Quick
        Qt6::QuickControls2
        Qt6::Test
    )
    target_compile_definitions(test_work_v2_checkin_clicks PRIVATE
        ISVN_GUI_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}"
    )
    target_compile_options(test_work_v2_checkin_clicks PRIVATE -Wall -Wextra -Wpedantic)
    add_test(NAME gui_work_v2_checkin_clicks COMMAND test_work_v2_checkin_clicks)
    set_tests_properties(gui_work_v2_checkin_clicks PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
    )

# The Import controller is exercised against a stdin-aware fake
    # bridge.  This guards Drop -> Preview -> Confirm -> Apply, per-item
    # replacement, exact Batch hand-off, and source-path argv privacy.
    add_executable(test_work_v2_import_clicks
        tests/test_work_v2_import_clicks.cpp
    )
    target_link_libraries(test_work_v2_import_clicks PRIVATE
        Qt6::Core
        Qt6::Gui
        Qt6::Qml
        Qt6::Quick
        Qt6::QuickControls2
        Qt6::Test
    )
    target_compile_definitions(test_work_v2_import_clicks PRIVATE
        ISVN_GUI_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}"
    )
    target_compile_options(test_work_v2_import_clicks PRIVATE -Wall -Wextra -Wpedantic)
    add_test(NAME gui_work_v2_import_clicks COMMAND test_work_v2_import_clicks)
    set_tests_properties(gui_work_v2_import_clicks PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
    )

    add_executable(test_work_v2_operation_coordinator_clicks
        tests/test_work_v2_operation_coordinator_clicks.cpp
    )
    target_link_libraries(test_work_v2_operation_coordinator_clicks PRIVATE
        Qt6::Core
        Qt6::Gui
        Qt6::Qml
        Qt6::Quick
        Qt6::QuickControls2
        Qt6::Test
    )
    target_compile_definitions(test_work_v2_operation_coordinator_clicks PRIVATE
        ISVN_GUI_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}"
    )
    target_compile_options(test_work_v2_operation_coordinator_clicks PRIVATE -Wall -Wextra -Wpedantic)
    add_test(NAME gui_work_v2_operation_coordinator_clicks COMMAND test_work_v2_operation_coordinator_clicks)
    set_tests_properties(gui_work_v2_operation_coordinator_clicks PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
    )

    # Test/development-only preview & screenshot host. It is NOT installed and is
    # NOT a production entry point; it loads gui/qml/shell/WorkPreviewHost.qml
    # from the source tree. No add_test: screens are rendered by running the
    # host by hand, not by ctest.
    add_executable(work_preview
        tests/work_preview_main.cpp
    )
    target_link_libraries(work_preview PRIVATE
        isvn_gui_backend
        Qt6::Gui
        Qt6::Qml
        Qt6::Quick
        Qt6::QuickControls2
    )
    target_compile_definitions(work_preview PRIVATE
        ISVN_GUI_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}"
    )
    target_compile_options(work_preview PRIVATE -Wall -Wextra -Wpedantic)
endif()
