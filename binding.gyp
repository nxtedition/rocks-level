{
    "variables": {"openssl_fips": "0"},
    "targets": [
        {
            "target_name": "leveldown",
            "defines": ["BOOST_REGEX_STANDALONE=yes"],
            "conditions": [
                [
                    "OS == 'linux'",
                    {
                        "cflags": ["-march=znver1"],
                        "include_dirs": [
                            "/usr/lib/x86_64-linux-gnu/include",
                            "/usr/lib/include",
                        ],
                        "ccflags": ["-flto"],
                        "cflags!": ["-fno-exceptions"],
                        "cflags_cc!": ["-fno-exceptions"],
                        "ldflags": ["-flto", "-fuse-linker-plugin"],
                    },
                ],
                [
                    "OS == 'mac'",
                    {
                        "xcode_settings": {
                            "WARNING_CFLAGS": [
                                "-Wno-sign-compare",
                                "-Wno-unused-variable",
                                "-Wno-unused-function",
                                "-Wno-ignored-qualifiers",
                            ],
                            "OTHER_CPLUSPLUSFLAGS": [
                                "-mmacosx-version-min=13.4.0",
                                "-std=c++20",
                                "-fno-omit-frame-pointer",
                                "-momit-leaf-frame-pointer",
                                "-arch x86_64",
                                "-arch arm64",
                            ],
                            "GCC_ENABLE_CPP_RTTI": "YES",
                            "GCC_ENABLE_CPP_EXCEPTIONS": "YES",
                            "MACOSX_DEPLOYMENT_TARGET": "13.4.0",
                        }
                    },
                ],
            ],
            "dependencies": ["<(module_root_dir)/deps/rocksdb/rocksdb.gyp:rocksdb"],
            "include_dirs": ["<!(node -e \"require('napi-macros')\")"],
            "sources": ["binding.cc"],
        }
    ],
}
