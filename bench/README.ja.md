# ベンチマーク

[English](README.md) | 日本語

ここと[性能](../README.ja.md#性能)の表は `bench/run.py` で計測したものです。このスクリプトは、各プログラムをすべての処理系で同じ入力で実行し、出力を検証して、起動時間を含む実時間を報告します。20 秒未満の実行は3回の中央値、それ以外は1回の値です。実行は 600 秒で打ち切ります。

プログラム：

- **Brainfuck インタプリタ**：Piet のサンプルページにある [piet_bfi.gif](https://www.dangermouse.net/esoteric/piet/piet_bfi.gif) を、GIF を読めない処理系もあるので PNG に変換して使います。Daniel B Cristofani 氏の [squares.b](https://brainfuck.org/squares.b)（10,000 までの平方数を出力）と、64・10・10・10 回の4重ループを実行させます。どちらのファイルもスクリプトがダウンロードします。
- **アッカーマン関数と竹内関数**は `bench/programs.py` が生成します。どちらも呼び出しスタックを Piet のスタック上に持ちます。アッカーマン関数は未処理の m の値の列として持ち、A(3, 12) では 32,764 個まで伸びます。竹内関数は、戻り値ごとに次に何をするかを表すフレームとして持ちます。どちらもループの各回で、スタックの上の値によって分岐します。
- **残りの4つのプログラム**も `bench/programs.py` が生成します。どれも N を読んで1つのループを回します。c mod 7 の和、試し割りによる素数の数え上げ、深さ 3〜52 の roll による 50 個の値の読み出し、N 個の数値の出力です。

結果（単位は秒。実行は 600 秒で打ち切り。† は最適化なしの PietCC で、`--o2` ではコンパイルが30分で終わりませんでした）：

| プログラム | **Pietric** | PietCC | npiet | rpiet | piet_programming_language |
|---|---|---|---|---|---|
| Piet 製 Brainfuck インタプリタで `squares.b` を実行 | **0.095** | 5.5 † | 144 | 12.3 | 207 |
| Piet 製 Brainfuck インタプリタで4重ループを実行 | **0.023** | 0.50 † | 18.3 | 2.17 | 36.8 |
| アッカーマン関数 A(3, 12) | **0.343** | 43.5 | > 600 | > 600 | > 600 |
| 竹内関数 tak(26, 16, 8) | **0.589** | 6.21 | 292 | 127 | > 600 |
| c = 1〜10⁸ の c mod 7 の和 | **0.047** | 3.76 | 231 | 57.5 | > 600 |
| 試し割りで 500,000 までの素数を数える | **0.173** | 5.73 | 270 | 68.4 | > 600 |
| スタックの 3〜52 の深さの値を 10⁷ 回読む | **0.108** | 2.37 | 83.5 | 16.6 | 506 |
| 10⁷ 個の数値を出力 | **0.155** | 0.79 | 24.0 | 7.47 | 240 |

計測した処理系（Ubuntu 22.04 on WSL2、Intel Core i5-12400）：

| 処理系 | バージョン | ビルドと実行の方法 |
|---|---|---|
| Pietric | このリポジトリ | Release ビルド、`-O2`（既定） |
| [PietCC](https://github.com/pwang00/pietcc) | 26e8bec | LLVM 20 で `cargo build --release`（既定の LLVM 23 の代わりに `llvm20-1` フィーチャー）、`--o2` |
| [npiet](https://www.bertnase.de/npiet/) | 1.3f | `./configure && make`（`-O2`） |
| [rpiet](https://github.com/tessi/rpiet) | 0.3.0 (e6a10ba) | `cargo build --release`、`-c 1` |
| [piet_programming_language](https://github.com/your-diary/piet_programming_language) | 3526991 | `cargo build --release`、`-c 1` |

2つのコンパイラのコンパイル時間：

| プログラム | Pietric | PietCC |
|---|---|---|
| Brainfuck インタプリタ | 4.9 秒 | 最適化なしで 8.3 秒（`--o2` では30分で終わらず） |
| アッカーマン関数 | 0.27 秒 | 0.80 秒 |
| 竹内関数 | 5.0 秒 | 85.6 秒 |
| c mod 7 の和 | 0.09 秒 | 0.27 秒 |
| 素数 | 0.16 秒 | 3.3 秒 |
| スタックの深い値 | 0.30 秒 | 11.1 秒 |
| 数値の出力 | 0.10 秒 | 0.19 秒 |

ベンチマークを実行するには、リポジトリのルートで、手元にある処理系を指定します（Python 3 と Pillow が必要です。PietCC には `PATH` 上の `llc` と `clang` も必要です）。
```bash
bench/run.py --pietric build/Pietric --pietcc path/to/pietcc --npiet path/to/npiet \
    --rpiet path/to/rpiet --piet-rs path/to/piet_programming_language
```
