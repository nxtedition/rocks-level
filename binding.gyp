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
                        "direct_dependent_settings": {
                          "libraries": [
                          ],
                        },
                        "include_dirs": [
                          "/usr/lib/x86_64-linux-gnu/include",
                          "/usr/lib/include",
                        ],
                        "libraries": [
                          "/usr/local/lib/libre2.a",
                          "<!@(ls /usr/local/lib/libabsl_*.a)"
                        ],
                        "cflags": [],
                        "cflags_cc": ["-flto", "-std=c++23"],
                        "cflags!": ["-fno-exceptions"],
                        "cflags_cc!": ["-fno-exceptions"],
                        "ldflags": ["-flto", "-fuse-linker-plugin", "-Wl,--whole-archive,/usr/local/lib/libsnappy.a,--no-whole-archive"],
                    },
                ],
                [
                    "OS == 'mac'",
                    {
                        "direct_dependent_settings": {
                          "libraries": [
                          ],
                        },
                        "include_dirs": [
                          "/opt/homebrew/include",
                          "/usr/local/include"
                        ],
                        "libraries": [
                          "-L/opt/homebrew/lib",
                          "-L/usr/local/lib",
                          "-lre2"
                        ],
                        "xcode_settings": {
                            "WARNING_CFLAGS": [
                                "-Wno-sign-compare",
                                "-Wno-unused-variable",
                                "-Wno-unused-function",
                                "-Wno-ignored-qualifiers",
                            ],
                            "OTHER_CPLUSPLUSFLAGS": [
                                "-mmacosx-version-min=13.4.0",
                                "-std=c++23",
                                "-fno-omit-frame-pointer",
                                "-momit-leaf-frame-pointer",
                                "-arch x86_64",
                                "-arch arm64",
                            ],
                            "OTHER_LDFLAGS": [
                                "-L/opt/homebrew/lib",
                                "-L/usr/local/lib"
                            ],
                            "GCC_ENABLE_CPP_RTTI": "YES",
                            "GCC_ENABLE_CPP_EXCEPTIONS": "YES",
                            "MACOSX_DEPLOYMENT_TARGET": "13.4.0",
                        }
                    },
                ],
            ],
            "dependencies": ["<(module_root_dir)/deps/rocksdb/rocksdb.gyp:rocksdb"],
            "include_dirs": [
              "<!(node -e \"require('napi-macros')\")"
            ],
            "sources": [
              "binding.cc"
          ],
        }
    ],
}
