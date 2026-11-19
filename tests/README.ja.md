# テスト

[English](README.md) | 日本語

E2E テストは、`tests/cases/` の各プログラムを Pietric の `-O0` と `-O2` でコンパイル・実行し、出力を期待する出力と比べます。ビルド用ディレクトリで次を実行します。
```bash
ctest --output-on-failure
```
失敗を1つずつ見たいときは、スクリプトを直接呼びます（最後の引数を付けると、名前にそれを含むテストだけを実行します）。
```bash
../tests/run_tests.sh ./Pietric cc ../tests/cases
```

テストケース `<name>` は、プログラム（`<name>.txt` か画像）、期待する標準出力（`<name>.out`）、必要なら標準入力（`<name>.in`）からなります。テキスト形式のケースは `tests/gen_cases.py` が生成します。追加するには、`CASES` に期待する出力とともに項目を足し、`python3 tests/gen_cases.py` を実行します。
