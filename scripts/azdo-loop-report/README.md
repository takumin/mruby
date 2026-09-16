# Azure DevOps の進捗を Microsoft Loop に貼る

`azdo_loop_report.py` は、Azure DevOps の現在のスプリントの作業項目を取得し、親子階層を保った表として HTML と Markdown に書き出します。
HTML をブラウザで開いて表を選択しコピーすると、Loop に表として貼り付けられます。

Python 3.8 以上があれば動きます。追加パッケージは不要です。

## 準備

PAT に **Work Items: Read** のスコープを与え、環境変数に入れます。
`AZURE_DEVOPS_PAT`、`AZURE_DEVOPS_EXT_PAT`、`SYSTEM_ACCESSTOKEN` の順に探します。

```sh
export AZURE_DEVOPS_PAT='（発行した PAT）'
```

PowerShell の場合は次のとおりです。

```powershell
$env:AZURE_DEVOPS_PAT = '（発行した PAT）'
```

## 実行

```sh
python3 azdo_loop_report.py \
  --org contoso \
  --project MyProject \
  --team 'MyProject Team' \
  --out-dir ./out
```

`--org` は `https://dev.azure.com/<org>` の `<org>` 部分です。
`--team` は `@CurrentIteration`（現在のスプリント）の解決に使います。省略するとプロジェクトの既定チームのスプリントになります。

`./out/azdo-progress.html` と `./out/azdo-progress.md` が生成されます。

## Loop への貼り付け

1. 生成された `azdo-progress.html` をブラウザで開きます。
2. 表を選択してコピーします（表の左上から右下までドラッグ、またはページ全体を `Ctrl+A`）。
3. Loop のページに `Ctrl+V` で貼り付けます。表として取り込まれ、Loop 側でそのまま編集できます。

`Ctrl+Shift+V`（書式なし貼り付け）だと表が崩れるので、通常の貼り付けを使います。
Markdown 版は Teams のメッセージや GitHub など、表の HTML を受け付けない貼り付け先向けです。

## 出力の読み方

列は ID、タイトル、状態、担当、残工数です。

- タイトルは階層の深さだけ字下げされます。Epic や Feature のように子を持つ行は太字です。
- スプリントの外にいる親（Epic、Feature など）は、階層を示すために取得され、`(スプリント外)` と付きます。件数や進捗率の集計には入りません。
- 残工数は、子を持たない行では作業項目自身の値、子を持つ行では配下の合計です。
- 見出しの下に、件数、完了率、残工数の合計、状態別の内訳が並びます。

## オプション

| オプション         | 説明                                                                                    |
| ------------------ | --------------------------------------------------------------------------------------- |
| `--org`            | 組織名（必須）                                                                          |
| `--project`        | プロジェクト名（必須）                                                                  |
| `--team`           | チーム名。`@CurrentIteration` の解決に使う                                              |
| `--iteration-path` | スプリントを直接指定する（例: `'MyProject\Sprint 12'`）。現在のスプリントの代わりに使う |
| `--out-dir`        | 出力先ディレクトリ（既定: カレントディレクトリ）                                        |
| `--basename`       | 出力ファイル名の基底（既定: `azdo-progress`）                                           |
| `--heading`        | 表の見出し（既定: プロジェクト名とスプリント名）                                        |
| `--no-ancestors`   | スプリント外の親を取得せず、平坦な一覧にする                                            |
| `--done-state`     | 完了とみなす状態。複数回指定できる（既定: Done、Closed、Completed、Resolved）           |
| `--api-version`    | API バージョン（既定: 7.1）                                                             |

## うまくいかないとき

- **「JSON 以外の応答が返りました」**：PAT が失効しているか、スコープに Work Items: Read が入っていません。
- **「該当する作業項目がありませんでした」**：チーム名が違うか、現在のスプリントに作業項目がありません。`--iteration-path` でスプリントを直接指定すると切り分けられます。
- **状態の内訳が想定と違う**：プロセステンプレートによって状態名が変わります。`--done-state` で完了扱いの状態を指定してください。
