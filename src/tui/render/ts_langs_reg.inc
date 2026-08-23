// 本文件由 scripts/gen_ts_grammars.py 自动生成, 请勿手改. 改语言清单后重跑该脚本.

#ifdef WORKX_TS_BASH
            langs["bash"] = tree_sitter_bash();
            langs["sh"] = tree_sitter_bash();
            langs["shell"] = tree_sitter_bash();
            langs["zsh"] = tree_sitter_bash();
#endif
#ifdef WORKX_TS_C
            langs["c"] = tree_sitter_c();
#endif
#ifdef WORKX_TS_CPP
            langs["cpp"] = tree_sitter_cpp();
            langs["c++"] = tree_sitter_cpp();
            langs["cxx"] = tree_sitter_cpp();
            langs["cc"] = tree_sitter_cpp();
            langs["h"] = tree_sitter_cpp();
            langs["hpp"] = tree_sitter_cpp();
#endif
#ifdef WORKX_TS_GO
            langs["go"] = tree_sitter_go();
            langs["golang"] = tree_sitter_go();
#endif
#ifdef WORKX_TS_JAVASCRIPT
            langs["javascript"] = tree_sitter_javascript();
            langs["js"] = tree_sitter_javascript();
            langs["jsx"] = tree_sitter_javascript();
#endif
#ifdef WORKX_TS_JSON
            langs["json"] = tree_sitter_json();
#endif
#ifdef WORKX_TS_PYTHON
            langs["python"] = tree_sitter_python();
            langs["py"] = tree_sitter_python();
#endif
#ifdef WORKX_TS_RUST
            langs["rust"] = tree_sitter_rust();
            langs["rs"] = tree_sitter_rust();
#endif
#ifdef WORKX_TS_TSX
            langs["tsx"] = tree_sitter_tsx();
#endif
#ifdef WORKX_TS_TYPESCRIPT
            langs["typescript"] = tree_sitter_typescript();
            langs["ts"] = tree_sitter_typescript();
#endif
