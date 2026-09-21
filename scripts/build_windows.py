import os
import subprocess
import glob
import sys

def main():
    build_dir = "build/c"
    os.makedirs(build_dir, exist_ok=True)

    # 1. Gather all C sources
    foundation_srcs = [
        "src/foundation/mem_override_win.c",
        "src/foundation/arena.c",
        "src/foundation/hash_table.c",
        "src/foundation/str_intern.c",
        "src/foundation/log.c",
        "src/foundation/str_util.c",
        "src/foundation/workspace.c",
        "src/foundation/platform.c",
        "src/foundation/system_info.c",
        "src/foundation/slab_alloc.c",
        "src/foundation/yaml.c",
        "src/foundation/compat.c",
        "src/foundation/compat_thread.c",
        "src/foundation/compat_fs.c",
        "src/foundation/compat_regex.c",
        "src/foundation/mem.c",
        "src/foundation/diagnostics.c",
        "src/foundation/profile.c",
        "src/foundation/dump_verify.c",
        "src/foundation/limits.c",
        "src/foundation/subprocess.c",
        "src/foundation/sha256.c",
        "src/foundation/secure_random.c",
        "src/foundation/macos_acl.c",
        "src/foundation/private_file_lock.c",
        "src/foundation/lock_registry.c",
        "src/foundation/http_client.c",
    ]

    extraction_srcs = [
        "internal/cbm/cbm.c",
        "internal/cbm/extract_defs.c",
        "internal/cbm/extract_calls.c",
        "internal/cbm/extract_imports.c",
        "internal/cbm/extract_usages.c",
        "internal/cbm/extract_unified.c",
        "internal/cbm/extract_semantic.c",
        "internal/cbm/extract_type_refs.c",
        "internal/cbm/extract_type_assigns.c",
        "internal/cbm/extract_env_accesses.c",
        "internal/cbm/extract_channels.c",
        "internal/cbm/extract_k8s.c",
        "internal/cbm/extract_dbt.c",
        "internal/cbm/helpers.c",
        "internal/cbm/lang_specs.c",
        "internal/cbm/macro_table.c",
        "internal/cbm/iris_export_xml.c",
        "internal/cbm/service_patterns.c",
    ]

    ac_lz4_srcs = ["internal/cbm/ac.c", "internal/cbm/lz4_store.c"]
    zstd_srcs = ["internal/cbm/zstd_store.c"]
    sqlite_writer_src = ["internal/cbm/sqlite_writer.c"]

    store_srcs = ["src/store/store.c", "src/store/history_store.c"]
    cypher_srcs = ["src/cypher/cypher.c"]
    mcp_srcs = [
        "src/mcp/mcp.c",
        "src/mcp/index_supervisor.c",
        "src/mcp/compact_out.c",
        "src/mcp/event_ingest.c",
        "src/mcp/change_correlation.c",
        "src/mcp/failure_correlation.c",
        "src/mcp/correction_memory.c",
        "src/mcp/orchestrator.c",
    ]
    daemon_srcs = [
        "src/daemon/daemon.c",
        "src/daemon/project_lock.c",
        "src/daemon/version_cohort.c",
        "src/daemon/service.c",
        "src/daemon/runtime.c",
        "src/daemon/application.c",
        "src/daemon/frontend.c",
        "src/daemon/host.c",
        "src/daemon/bootstrap.c",
        "src/daemon/ipc.c",
    ]
    discover_srcs = [
        "src/discover/language.c",
        "src/discover/userconfig.c",
        "src/discover/gitignore.c",
        "src/discover/discover.c",
    ]
    graph_buffer_srcs = ["src/graph_buffer/graph_buffer.c"]
    pipeline_srcs = [
        "src/pipeline/fqn.c",
        "src/pipeline/lsp_surface.c",
        "src/pipeline/pipeline_delta.c",
        "src/pipeline/path_alias.c",
        "src/pipeline/registry.c",
        "src/pipeline/pipeline.c",
        "src/pipeline/pipeline_incremental.c",
        "src/pipeline/worker_pool.c",
        "src/pipeline/pass_parallel.c",
        "src/pipeline/pass_definitions.c",
        "src/pipeline/pass_calls.c",
        "src/pipeline/pass_lsp_cross.c",
        "src/pipeline/pass_usages.c",
        "src/pipeline/pass_semantic.c",
        "src/pipeline/pass_tests.c",
        "src/pipeline/pass_githistory.c",
        "src/pipeline/pass_gitdiff.c",
        "src/pipeline/pass_configures.c",
        "src/pipeline/pass_configlink.c",
        "src/pipeline/pass_route_nodes.c",
        "src/pipeline/pass_enrichment.c",
        "src/pipeline/pass_envscan.c",
        "src/pipeline/pass_compile_commands.c",
        "src/pipeline/pass_infrascan.c",
        "src/pipeline/pass_k8s.c",
        "src/pipeline/pass_similarity.c",
        "src/pipeline/pass_semantic_edges.c",
        "src/pipeline/pass_complexity.c",
        "src/pipeline/pass_cross_repo.c",
        "src/pipeline/artifact.c",
        "src/pipeline/pass_pkgmap.c",
    ]
    simhash_srcs = ["src/simhash/minhash.c"]
    semantic_srcs = ["src/semantic/semantic.c", "src/semantic/ast_profile.c", "src/semantic/rotsq.c"]
    traces_srcs = ["src/traces/traces.c"]
    database_srcs = [
        "src/database/db_introspect.c",
        "src/database/db_sqlite_introspect.c",
        "src/database/db_postgres_introspect.c",
        "src/database/db_mysql_introspect.c",
        "src/database/db_mongodb_introspect.c",
        "src/database/erd_generator.c",
    ]
    watcher_srcs = ["src/watcher/watcher.c"]
    git_srcs = ["src/git/git_context.c"]
    cli_srcs = [
        "src/cli/cli.c",
        "src/cli/progress_sink.c",
        "src/cli/hook_augment.c",
        "src/cli/client_adapter.c",
        "src/cli/agent_clients.c",
        "src/cli/agent_profiles.c",
        "src/cli/config_json_like.c",
        "src/cli/config_toml_edit.c",
        "src/cli/config_yaml_edit.c",
        "src/cli/config_text_edit.c",
        "src/cli/activation_transaction.c",
    ]
    ui_srcs = [
        "src/ui/config.c",
        "src/ui/http_server.c",
        "src/ui/layout3d.c",
        "src/ui/httpd.c",
        "src/ui/embedded_stub.c",
    ]
    yyjson_src = ["vendored/yyjson/yyjson.c"]
    main_src = ["src/main.c"]

    all_c_sources = (
        main_src
        + foundation_srcs
        + store_srcs
        + cypher_srcs
        + mcp_srcs
        + daemon_srcs
        + discover_srcs
        + graph_buffer_srcs
        + pipeline_srcs
        + simhash_srcs
        + semantic_srcs
        + traces_srcs
        + database_srcs
        + watcher_srcs
        + git_srcs
        + cli_srcs
        + ui_srcs
        + yyjson_src
        + extraction_srcs
        + ac_lz4_srcs
        + zstd_srcs
        + sqlite_writer_src
    )

    # 2. Gather all pre-compiled object files in build/c
    obj_files = glob.glob(f"{build_dir}/prod_*.o") + [f"{build_dir}/unixcoder_blob.o"]

    # 3. Write response file
    rsp_file = os.path.join(build_dir, "link_cbm.rsp")
    flags = [
        "-std=c11",
        "-D_DEFAULT_SOURCE",
        "-D_GNU_SOURCE",
        "-Wall",
        "-Wextra",
        "-Wno-unused-parameter",
        "-Wno-sign-compare",
        "-Wdate-time",
        "-Wno-format-truncation",
        "-Wno-unused-result",
        "-Wno-stringop-truncation",
        "-Wno-alloc-size-larger-than",
        "-Isrc",
        "-Ivendored",
        "-Ivendored/yyjson",
        "-Ivendored/sqlite3",
        "-Ivendored/mimalloc/include",
        "-Iinternal/cbm",
        "-Iinternal/cbm/vendored/ts_runtime/include",
        "-Ibuild/c/generated",
        "-O2",
        "-DCBM_BIND_TS_ALLOCATOR=1",
        "-DMI_MALLOC_OVERRIDE=1",
        "-DCBM_MEM_GLOBAL_OVERRIDE=1",
        "-o",
        f"{build_dir}/codebase-memory-mcp.exe",
    ]

    wrap_syms = ["malloc", "calloc", "realloc", "free", "strdup", "strndup", "_msize", "_aligned_malloc", "_aligned_free"]
    wrap_flags = [f"-Wl,--wrap={sym}" for sym in wrap_syms]

    libs = [
        "-lm",
        "-lstdc++",
        "-lpthread",
        "-lws2_32",
        "-lpsapi",
        "-lshell32",
        "-ladvapi32",
        "-lbcrypt",
        "-Wl,--allow-multiple-definition",
        "-Wl,--stack,8388608",
        "-Wl,--no-insert-timestamp",
        "-static",
    ] + wrap_flags

    with open(rsp_file, "w", encoding="utf-8") as f:
        for item in flags:
            f.write(f'"{item}"\n')
        for item in all_c_sources:
            f.write(f'"{item}"\n')
        for item in obj_files:
            f.write(f'"{item.replace(os.sep, "/")}"\n')
        for item in libs:
            f.write(f'"{item}"\n')

    print(f"Generated response file: {rsp_file}")
    print(f"Linking codebase-memory-mcp.exe with gcc...")

    cmd = ["gcc", f"@{rsp_file}"]
    res = subprocess.run(cmd)
    if res.returncode != 0:
        print(f"Link failed with code {res.returncode}")
        sys.exit(res.returncode)

    print(f"SUCCESS: Built {build_dir}/codebase-memory-mcp.exe")

if __name__ == "__main__":
    main()
