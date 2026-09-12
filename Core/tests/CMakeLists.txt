add_subdirectory(fixtures/plugin_backend_fixture)
add_subdirectory(fixtures/argv_echo)

add_executable(aistudio_core_tests
    test_main.cpp
    test_result.cpp
    test_config.cpp
    test_backend_registry.cpp
    test_capability_version.cpp
    test_backend_factory_registry.cpp
    test_plugin_registry.cpp
    test_plugin_loader.cpp
    test_plugin_backend_adapter.cpp
    test_plugin_permissions.cpp
    test_plugin_bootstrap.cpp
    test_symbol_extractor.cpp
    test_symbol_index.cpp
    test_symbol_context_source.cpp
    test_include_extractor.cpp
    test_include_graph.cpp
    test_dependency_context_source.cpp
    test_call_extractor.cpp
    test_call_graph.cpp
    test_impact_analyzer.cpp
    test_identifier.cpp
    test_utf16.cpp
    test_utf8.cpp
    test_text_range.cpp
    test_inheritance_extractor.cpp
    test_inheritance_graph.cpp
    test_reference_extractor.cpp
    test_reference_graph.cpp
    test_ast_extractor.cpp
    test_ast_index.cpp
    test_ast_context_source.cpp
    test_keyword_search.cpp
    test_symbol_search.cpp
    test_echo_llm_provider.cpp
    test_context_retriever.cpp
    test_context_restorer.cpp
    test_context_garbage_collector.cpp
    test_context_usage_summary.cpp
    test_embedding_cache.cpp
    test_summary_cache.cpp
    test_context_cache.cpp
    test_sent_ledger.cpp
    test_file_provider_backend.cpp
    test_project_rules_backend.cpp
    test_task_queue.cpp
    test_task_executor.cpp
    test_event_bus.cpp
    test_protocol.cpp
    test_database.cpp
    test_migrator.cpp
    test_task_repository.cpp
    test_session_repository.cpp
    test_cache.cpp
    test_api_server.cpp
    test_mcp_server.cpp
    test_lsp_server.cpp
    test_editor_state_store.cpp
    test_file_scanner.cpp
    test_file_cache.cpp
    test_file_watcher.cpp
    test_context_item.cpp
    test_context_budget.cpp
    test_context_compressor.cpp
    test_context_selector.cpp
    test_context_snapshot.cpp
    test_context_snapshot_repository.cpp
    test_context_audit_entry.cpp
    test_context_audit_repository.cpp
    test_glob.cpp
    test_permission_policy.cpp
    test_approval_queue.cpp
    test_sandbox.cpp
    test_secret.cpp
    test_file_context_source.cpp
    test_process_runner.cpp
    test_pty_process.cpp
    test_generic_cli_adapter.cpp
    test_cli_diagnostics.cpp
    test_operation_probes.cpp
    test_handoff_resume_orchestrator.cpp
    test_session_manager.cpp
    test_worktree_mcp_config.cpp
    test_resource_lease_ledger.cpp
    test_handoff_record.cpp
    test_handoff_verifier.cpp
    test_handoff_repository.cpp
    test_operation_ledger.cpp
    test_operation_ledger_repository.cpp
    test_comparison_board.cpp
    test_comparison_trial_repository.cpp
    test_workspace_hash.cpp
    test_git_backend.cpp
)

target_link_libraries(aistudio_core_tests PRIVATE aistudio_core)

# Not a link dependency (test_plugin_backend_adapter.cpp loads these at
# runtime via PluginLoader, the same way a real Plugin DLL would be
# loaded) -- add_dependencies just ensures ctest/the build always builds
# them first, and the genex compile definitions hand the test binary
# their actual output paths (which vary by generator/config) without
# hardcoding anything.
add_dependencies(aistudio_core_tests aistudio_plugin_fixture aistudio_plugin_fixture_broken aistudio_argv_echo_fixture)
target_compile_definitions(aistudio_core_tests PRIVATE
    AISTUDIO_PLUGIN_FIXTURE_PATH="$<TARGET_FILE:aistudio_plugin_fixture>"
    AISTUDIO_PLUGIN_FIXTURE_BROKEN_PATH="$<TARGET_FILE:aistudio_plugin_fixture_broken>"
    AISTUDIO_ARGV_ECHO_FIXTURE_PATH="$<TARGET_FILE:aistudio_argv_echo_fixture>"
)

add_test(NAME aistudio_core_tests COMMAND aistudio_core_tests)
