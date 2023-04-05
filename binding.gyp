{
    "variables": {"openssl_fips": "0"},
    "targets": [
        {
            "target_name": "leveldown",
            "conditions": [
                [
                    "OS == 'linux'",
                    {
                        "cflags": [
                            "-msse4.2",
                            "-mpclmul",
                            "-mavx",
                            "-mavx2",
                            "-mbmi",
                            "-mlzcnt"
                        ],
                        "ccflags": ["-flto"],
                        "cflags!": ["-fno-exceptions"],
                        "cflags_cc!": ["-fno-exceptions"],
                        "ldflags": ["-flto", "-fuse-linker-plugin"]
                    }
                ],
                [
                    "OS == 'mac'",
                    {
                        "xcode_settings": {
                            "WARNING_CFLAGS": [
                                "-Wno-sign-compare",
                                "-Wno-unused-variable",
                                "-Wno-unused-function",
                                "-Wno-ignored-qualifiers"
                            ],
                            "OTHER_CPLUSPLUSFLAGS": [
                                "-mmacosx-version-min=12.2.1",
                                "-std=c++20",
                                "-fno-omit-frame-pointer",
                                "-momit-leaf-frame-pointer",
                                "-arch x86_64",
                                "-arch arm64"
                            ],
                            "GCC_ENABLE_CPP_RTTI": "YES",
                            "GCC_ENABLE_CPP_EXCEPTIONS": "YES",
                            "MACOSX_DEPLOYMENT_TARGET": "12.2.1"
                        }
                    }
                ]
            ],
            "dependencies": [
				"<(module_root_dir)/deps/rocksdb/rocksdb.gyp:rocksdb",
				"<(module_root_dir)/deps/liburing/liburing.gyp:liburing"
            ],
            "include_dirs": ["<!(node -e \"require('napi-macros')\")",],
            "sources": ["binding.cc"]
        }
    ]
}
