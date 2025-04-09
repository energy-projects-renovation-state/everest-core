load("@bazel_tools//tools/build_defs/repo:http.bzl", "http_archive")
load("@bazel_tools//tools/build_defs/repo:git.bzl", "git_repository")
load("@bazel_tools//tools/build_defs/repo:utils.bzl", "maybe")
load("@rules_rust//crate_universe:defs.bzl", "crates_repository", "crate")
load("@rules_rust//crate_universe:repositories.bzl", "crate_universe_dependencies")
load("@everest-core//third-party/bazel:edm.bzl", "edm_repositories")


def everest_core_repos():
    http_archive(
        name = "rules_foreign_cc",
        sha256 = "476303bd0f1b04cc311fc258f1708a5f6ef82d3091e53fd1977fa20383425a6a",
        strip_prefix = "rules_foreign_cc-0.10.1",
        url = "https://github.com/bazelbuild/rules_foreign_cc/releases/download/0.10.1/rules_foreign_cc-0.10.1.tar.gz",
    )

    maybe(
        http_archive,
        name = "com_github_nelhage_rules_boost",
        url = "https://github.com/nelhage/rules_boost/archive/ec53a3b510be6a9fb41508cf884fe62d4098c5eb.tar.gz",
        sha256 = "9c3f4293b2d223cca4154e6fe69615bf870ba522fd8b6a7c7d438bbfd1de229f",
        strip_prefix = "rules_boost-ec53a3b510be6a9fb41508cf884fe62d4098c5eb",
    )

    crates_repository(
        name = "everest_core_crate_index",
        cargo_lockfile = "@everest-core//modules:Cargo.lock",
        isolated = False,
        manifests = [
            "@everest-core//modules:Cargo.toml",
            "@everest-core//modules/RsIskraMeter:Cargo.toml",
            "@everest-core//modules/RsPaymentTerminal:Cargo.toml",
            "@everest-core//modules/rust_examples/RsExample:Cargo.toml",
            "@everest-core//modules/rust_examples/RsExampleUser:Cargo.toml",
        ],
        annotations = {
            "everestrs": [crate.annotation(
                crate_features = ["build_bazel"],
            )],
        },
    )
    crate_universe_dependencies()

    maybe(
        http_archive,
        name = "rules_python",
        sha256 = "9acc0944c94adb23fba1c9988b48768b1bacc6583b52a2586895c5b7491e2e31",
        strip_prefix = "rules_python-0.27.0",
        url = "https://github.com/bazelbuild/rules_python/releases/download/0.27.0/rules_python-0.27.0.tar.gz",
    )

    edm_repositories(
        name = "edm_deps",
        dependencies_yaml = "@everest-core//:dependencies.yaml",
        build_files = [
            "@everest-core//third-party/bazel:BUILD.libtimer.bazel",
            "@everest-core//third-party/bazel:BUILD.pugixml.bazel",
            "@everest-core//third-party/bazel:BUILD.sigslot.bazel",
        ],
    )
