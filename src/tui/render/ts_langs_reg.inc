// 本文件由 scripts/gen_ts_grammars.py 自动生成, 请勿手改. 改语言清单后重跑该脚本.

#ifdef WORKX_TS_BASH
            langs["bash"] = tree_sitter_bash();
#endif
#ifdef WORKX_TS_C
            langs["c"] = tree_sitter_c();
#endif
#ifdef WORKX_TS_CPP
            langs["cpp"] = tree_sitter_cpp();
            langs["c++"] = tree_sitter_cpp();
#endif
#ifdef WORKX_TS_JSON
            langs["json"] = tree_sitter_json();
#endif
