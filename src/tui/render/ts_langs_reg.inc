// 本文件由 scripts/gen_ts_grammars.py 自动生成, 请勿手改. 改语言清单后重跑该脚本.

#ifdef WORKX_TS_BASH
            langs["bash"] = tree_sitter_bash();
#endif
#ifdef WORKX_TS_C
            langs["c"] = tree_sitter_c();
#endif
#ifdef WORKX_TS_C_SHARP
            langs["c-sharp"] = tree_sitter_c_sharp();
#endif
#ifdef WORKX_TS_CMAKE
            langs["cmake"] = tree_sitter_cmake();
#endif
#ifdef WORKX_TS_CPP
            langs["cpp"] = tree_sitter_cpp();
            langs["c++"] = tree_sitter_cpp();
#endif
#ifdef WORKX_TS_CSS
            langs["css"] = tree_sitter_css();
#endif
#ifdef WORKX_TS_DART
            langs["dart"] = tree_sitter_dart();
#endif
#ifdef WORKX_TS_DOCKERFILE
            langs["dockerfile"] = tree_sitter_dockerfile();
            langs["docker"] = tree_sitter_dockerfile();
#endif
#ifdef WORKX_TS_GO
            langs["go"] = tree_sitter_go();
#endif
#ifdef WORKX_TS_HASKELL
            langs["haskell"] = tree_sitter_haskell();
#endif
#ifdef WORKX_TS_HTML
            langs["html"] = tree_sitter_html();
#endif
#ifdef WORKX_TS_INI
            langs["ini"] = tree_sitter_ini();
#endif
#ifdef WORKX_TS_JAVA
            langs["java"] = tree_sitter_java();
#endif
#ifdef WORKX_TS_JAVASCRIPT
            langs["javascript"] = tree_sitter_javascript();
#endif
#ifdef WORKX_TS_JSON
            langs["json"] = tree_sitter_json();
#endif
#ifdef WORKX_TS_KOTLIN
            langs["kotlin"] = tree_sitter_kotlin();
#endif
#ifdef WORKX_TS_LUA
            langs["lua"] = tree_sitter_lua();
#endif
#ifdef WORKX_TS_MAKE
            langs["make"] = tree_sitter_make();
#endif
#ifdef WORKX_TS_MARKDOWN
            langs["markdown"] = tree_sitter_markdown();
            langs["md"] = tree_sitter_markdown();
#endif
#ifdef WORKX_TS_NIX
            langs["nix"] = tree_sitter_nix();
#endif
#ifdef WORKX_TS_PERL
            langs["perl"] = tree_sitter_perl();
#endif
#ifdef WORKX_TS_PHP
            langs["php"] = tree_sitter_php();
#endif
#ifdef WORKX_TS_PYTHON
            langs["python"] = tree_sitter_python();
#endif
#ifdef WORKX_TS_RUBY
            langs["ruby"] = tree_sitter_ruby();
#endif
#ifdef WORKX_TS_RUST
            langs["rust"] = tree_sitter_rust();
#endif
#ifdef WORKX_TS_SCALA
            langs["scala"] = tree_sitter_scala();
#endif
#ifdef WORKX_TS_SWIFT
            langs["swift"] = tree_sitter_swift();
#endif
#ifdef WORKX_TS_TOML
            langs["toml"] = tree_sitter_toml();
#endif
#ifdef WORKX_TS_TSX
            langs["tsx"] = tree_sitter_tsx();
#endif
#ifdef WORKX_TS_TYPESCRIPT
            langs["typescript"] = tree_sitter_typescript();
#endif
#ifdef WORKX_TS_YAML
            langs["yaml"] = tree_sitter_yaml();
            langs["yml"] = tree_sitter_yaml();
#endif
