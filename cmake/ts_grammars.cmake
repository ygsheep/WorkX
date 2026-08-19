# 本文件由 scripts/gen_ts_grammars.py 自动生成, 请勿手改. 改语言清单后重跑该脚本.

if(WORKX_HAS_TREE_SITTER AND WORKX_FETCH_GRAMMARS)
    message(STATUS "Fetching tree-sitter grammars:")
    workx_fetch_ts_grammar(bash           "https://github.com/tree-sitter/tree-sitter-bash"  "a06c2e4415e9bc0346c6b86d401879ffb44058f7")
    workx_fetch_ts_grammar(c              "https://github.com/tree-sitter/tree-sitter-c"  "b780e47fc780ddc8da13afa35a3f4ed5c157823d")
    workx_fetch_ts_grammar(cpp            "https://github.com/tree-sitter/tree-sitter-cpp"  "8b5b49eb196bec7040441bee33b2c9a4838d6967")
    workx_fetch_ts_grammar(json           "https://github.com/tree-sitter/tree-sitter-json"  "254c42a6476413b776221e03982ac8ae159eeb72")
endif()
