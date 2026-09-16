#!/usr/bin/env python3
"""Azure DevOps の作業項目を Microsoft Loop に貼り付けられる表として書き出す。

現在のスプリント (@CurrentIteration) の作業項目を取得し、親 (Epic / Feature など)
をたどって階層を復元したうえで、HTML と Markdown の表を出力する。
HTML をブラウザで開いて全選択コピーし、Loop に貼り付けると表として取り込まれる。

Python 3.8 以上の標準ライブラリのみで動作する。

使い方:
    export AZURE_DEVOPS_PAT='...'
    python3 azdo_loop_report.py --org contoso --project MyProject --team 'MyProject Team'
"""

from __future__ import annotations

import argparse
import base64
import datetime as _dt
import html
import json
import os
import sys
import urllib.error
import urllib.parse
import urllib.request
from typing import Dict, Iterable, List, Optional, Sequence

API_VERSION = "7.1"
BATCH_SIZE = 200

# Azure DevOps の Work Item Tracking API のパス。パスに含まれる綴りは API 側の名前で
# あり誤記ではないため、codespell の指摘を行単位で抑止している。
WIQL_PATH = "/_apis/wit/wiql"  # codespell:ignore wit
BATCH_PATH = "/_apis/wit/workitemsbatch"  # codespell:ignore wit

FIELDS = [
    "System.Id",
    "System.Title",
    "System.State",
    "System.WorkItemType",
    "System.AssignedTo",
    "System.Parent",
    "System.IterationPath",
    "Microsoft.VSTS.Scheduling.RemainingWork",
]

# 完了とみなす状態。--done-state で上書きできる。
DEFAULT_DONE_STATES = ["Done", "Closed", "Completed", "Resolved"]

# 同じ階層内の並び順。ここにない種別は最後に回す。
TYPE_ORDER = [
    "Epic",
    "Feature",
    "User Story",
    "Product Backlog Item",
    "Requirement",
    "Issue",
    "Bug",
    "Task",
    "Test Case",
]

PAT_ENV_VARS = ["AZURE_DEVOPS_PAT", "AZURE_DEVOPS_EXT_PAT", "SYSTEM_ACCESSTOKEN"]


class AzureDevOpsError(RuntimeError):
    pass


# --------------------------------------------------------------------------
# API クライアント
# --------------------------------------------------------------------------


class Client:
    def __init__(self, org: str, project: str, pat: str, api_version: str = API_VERSION):
        self.base = "https://dev.azure.com/{}/{}".format(
            urllib.parse.quote(org), urllib.parse.quote(project)
        )
        self.api_version = api_version
        token = base64.b64encode((":" + pat).encode("utf-8")).decode("ascii")
        self.headers = {
            "Authorization": "Basic " + token,
            "Content-Type": "application/json",
            "Accept": "application/json",
        }

    def post(self, path: str, payload: dict) -> dict:
        url = "{}{}?api-version={}".format(self.base, path, self.api_version)
        data = json.dumps(payload).encode("utf-8")
        req = urllib.request.Request(url, data=data, headers=self.headers, method="POST")
        try:
            with urllib.request.urlopen(req) as res:
                body = res.read().decode("utf-8")
                content_type = res.headers.get("Content-Type", "")
        except urllib.error.HTTPError as exc:
            detail = exc.read().decode("utf-8", "replace")[:500]
            raise AzureDevOpsError(
                "Azure DevOps API がエラーを返しました ({} {}): {}\nURL: {}".format(
                    exc.code, exc.reason, detail, url
                )
            ) from exc
        except urllib.error.URLError as exc:
            raise AzureDevOpsError(
                "Azure DevOps に接続できませんでした: {}\nURL: {}".format(exc.reason, url)
            ) from exc

        if "application/json" not in content_type:
            # 認証に失敗すると JSON ではなくサインインページが返ることがある。
            raise AzureDevOpsError(
                "JSON 以外の応答が返りました。PAT が無効か、権限 (Work Items: Read) が"
                "足りない可能性があります。\nURL: {}".format(url)
            )
        return json.loads(body)


# --------------------------------------------------------------------------
# 作業項目の取得
# --------------------------------------------------------------------------


def build_wiql(project: str, team: Optional[str], iteration_path: Optional[str]) -> str:
    if iteration_path:
        condition = "[System.IterationPath] UNDER '{}'".format(iteration_path.replace("'", "''"))
    elif team:
        condition = "[System.IterationPath] = @CurrentIteration('[{}]\\{}')".format(
            project.replace("'", "''"), team.replace("'", "''")
        )
    else:
        condition = "[System.IterationPath] = @CurrentIteration"
    return (
        "SELECT [System.Id] FROM WorkItems "
        "WHERE [System.TeamProject] = @project AND {} "
        "ORDER BY [System.Id]".format(condition)
    )


def query_ids(client: Client, team: Optional[str], wiql: str) -> List[int]:
    path = WIQL_PATH
    if team:
        path = "/{}{}".format(urllib.parse.quote(team), WIQL_PATH)
    result = client.post(path, {"query": wiql})
    return [int(item["id"]) for item in result.get("workItems", [])]


def fetch_work_items(client: Client, ids: Sequence[int]) -> Dict[int, dict]:
    items: Dict[int, dict] = {}
    for start in range(0, len(ids), BATCH_SIZE):
        chunk = list(ids[start : start + BATCH_SIZE])
        result = client.post(
            BATCH_PATH, {"ids": chunk, "fields": FIELDS, "errorPolicy": "omit"}
        )
        for raw in result.get("value", []):
            if raw is None:
                continue
            items[int(raw["id"])] = raw
    return items


def fetch_ancestors(client: Client, items: Dict[int, dict]) -> Dict[int, dict]:
    """スプリント外にいる親 (Epic / Feature など) を再帰的に取得して足す。"""
    known = dict(items)
    frontier = {
        int(raw["fields"]["System.Parent"])
        for raw in known.values()
        if raw["fields"].get("System.Parent")
    } - set(known)
    while frontier:
        fetched = fetch_work_items(client, sorted(frontier))
        if not fetched:
            break
        known.update(fetched)
        frontier = {
            int(raw["fields"]["System.Parent"])
            for raw in fetched.values()
            if raw["fields"].get("System.Parent")
        } - set(known)
    return known


# --------------------------------------------------------------------------
# 階層の組み立て
# --------------------------------------------------------------------------


class Node:
    __slots__ = ("id", "fields", "in_scope", "children", "org", "project")

    def __init__(self, raw: dict, in_scope: bool, org: str, project: str):
        self.id = int(raw["id"])
        self.fields = raw.get("fields", {})
        self.in_scope = in_scope
        self.children: List["Node"] = []
        self.org = org
        self.project = project

    # -- フィールドの読み出し ------------------------------------------------

    @property
    def title(self) -> str:
        return self.fields.get("System.Title", "")

    @property
    def state(self) -> str:
        return self.fields.get("System.State", "")

    @property
    def work_item_type(self) -> str:
        return self.fields.get("System.WorkItemType", "")

    @property
    def assigned_to(self) -> str:
        person = self.fields.get("System.AssignedTo")
        if isinstance(person, dict):
            return person.get("displayName") or person.get("uniqueName") or ""
        return person or ""

    @property
    def parent_id(self) -> Optional[int]:
        parent = self.fields.get("System.Parent")
        return int(parent) if parent else None

    @property
    def remaining_work(self) -> Optional[float]:
        value = self.fields.get("Microsoft.VSTS.Scheduling.RemainingWork")
        return float(value) if isinstance(value, (int, float)) else None

    @property
    def url(self) -> str:
        return "https://dev.azure.com/{}/{}/_workitems/edit/{}".format(
            urllib.parse.quote(self.org), urllib.parse.quote(self.project), self.id
        )

    # -- 集計 ---------------------------------------------------------------

    def subtree_remaining(self) -> Optional[float]:
        total = self.remaining_work
        for child in self.children:
            child_total = child.subtree_remaining()
            if child_total is not None:
                total = child_total if total is None else total + child_total
        return total

    def sort_key(self):
        try:
            rank = TYPE_ORDER.index(self.work_item_type)
        except ValueError:
            rank = len(TYPE_ORDER)
        return (rank, self.state, self.id)


def build_forest(known: Dict[int, dict], scope: Iterable[int], org: str, project: str) -> List[Node]:
    scope_ids = set(scope)
    nodes = {
        wid: Node(raw, wid in scope_ids, org, project) for wid, raw in known.items()
    }
    roots: List[Node] = []
    for node in nodes.values():
        parent = nodes.get(node.parent_id) if node.parent_id else None
        if parent is None:
            roots.append(node)
        else:
            parent.children.append(node)

    def sort_recursive(items: List[Node]) -> None:
        items.sort(key=Node.sort_key)
        for item in items:
            sort_recursive(item.children)

    sort_recursive(roots)
    return roots


def flatten(roots: Sequence[Node]) -> List["tuple[int, Node]"]:
    rows: List[tuple[int, Node]] = []

    def walk(node: Node, depth: int) -> None:
        rows.append((depth, node))
        for child in node.children:
            walk(child, depth + 1)

    for root in roots:
        walk(root, 0)
    return rows


# --------------------------------------------------------------------------
# 出力
# --------------------------------------------------------------------------


def format_hours(value: Optional[float]) -> str:
    if value is None:
        return ""
    if abs(value - round(value)) < 1e-9:
        return str(int(round(value)))
    return "{:.1f}".format(value)


def summarize(nodes: Sequence[Node], done_states: Sequence[str]) -> "tuple[List[tuple[str, int]], int, int, Optional[float]]":
    counts: Dict[str, int] = {}
    for node in nodes:
        counts[node.state] = counts.get(node.state, 0) + 1
    done = sum(count for state, count in counts.items() if state in done_states)
    remaining_values = [n.remaining_work for n in nodes if n.remaining_work is not None]
    remaining = sum(remaining_values) if remaining_values else None
    ordered = sorted(counts.items(), key=lambda kv: (-kv[1], kv[0]))
    return ordered, done, len(nodes), remaining


def format_summary(nodes: Sequence[Node], done_states: Sequence[str]) -> str:
    counts, done, total, remaining = summarize(nodes, done_states)
    parts = ["{} 件中 {} 件完了".format(total, done)]
    if total:
        parts[0] += " ({:.0f}%)".format(done * 100.0 / total)
    if remaining is not None:
        parts.append("残工数 {}".format(format_hours(remaining)))
    if counts:
        parts.append(
            "内訳: "
            + "、".join("{} {}".format(state or "(状態なし)", count) for state, count in counts)
        )
    return " ／ ".join(parts)


def md_cell(value: str) -> str:
    """Markdown の表のセルを壊す文字を逃がす。"""
    return value.replace("\\", "\\\\").replace("|", "\\|").replace("\n", " ")


def render_html(rows, scoped, heading: str, subtitle: str, done_states) -> str:
    summary = format_summary(scoped, done_states)

    out = [
        "<!DOCTYPE html>",
        '<html lang="ja"><head><meta charset="utf-8">',
        "<title>{}</title>".format(html.escape(heading)),
        "<style>",
        "body{font-family:'Segoe UI','Yu Gothic UI',sans-serif;margin:24px;color:#242424}",
        "table{border-collapse:collapse}",
        "th,td{border:1px solid #d1d1d1;padding:4px 8px;font-size:14px;text-align:left}",
        "th{background:#f3f2f1}",
        "td.num{text-align:right}",
        "</style></head><body>",
        "<h2>{}</h2>".format(html.escape(heading)),
        "<p>{}</p>".format(html.escape(subtitle)),
        "<p>{}</p>".format(html.escape(summary)),
        "<table>",
        "<thead><tr><th>ID</th><th>タイトル</th><th>状態</th><th>担当</th><th>残工数</th></tr></thead>",
        "<tbody>",
    ]
    for depth, node in rows:
        indent = "&nbsp;&nbsp;&nbsp;&nbsp;" * depth
        title = html.escape(node.title)
        if node.children:
            title = "<strong>{}</strong>".format(title)
        if not node.in_scope:
            title += ' <span style="color:#616161">(スプリント外)</span>'
        hours = node.subtree_remaining() if node.children else node.remaining_work
        out.append(
            "<tr>"
            '<td><a href="{url}">{id}</a></td>'
            "<td>{indent}{title}</td>"
            "<td>{state}</td><td>{assignee}</td>"
            '<td class="num">{hours}</td>'
            "</tr>".format(
                url=html.escape(node.url, quote=True),
                id=node.id,
                indent=indent,
                title=title,
                state=html.escape(node.state),
                assignee=html.escape(node.assigned_to),
                hours=format_hours(hours),
            )
        )
    out += ["</tbody></table>", "</body></html>", ""]
    return "\n".join(out)


def render_markdown(rows, scoped, heading: str, subtitle: str, done_states) -> str:
    summary = format_summary(scoped, done_states)

    lines = [
        "## {}".format(heading),
        "",
        subtitle,
        "",
        summary,
        "",
        "| ID | タイトル | 状態 | 担当 | 残工数 |",
        "| --- | --- | --- | --- | ---: |",
    ]
    for depth, node in rows:
        title = md_cell(node.title)
        if not node.in_scope:
            title += " (スプリント外)"
        hours = node.subtree_remaining() if node.children else node.remaining_work
        lines.append(
            "| [{id}]({url}) | {indent}{title} | {state} | {assignee} | {hours} |".format(
                id=node.id,
                url=node.url,
                indent="\u3000" * 2 * depth,
                title=title,
                state=md_cell(node.state),
                assignee=md_cell(node.assigned_to),
                hours=format_hours(hours),
            )
        )
    lines.append("")
    return "\n".join(lines)


# --------------------------------------------------------------------------
# エントリポイント
# --------------------------------------------------------------------------


def read_pat() -> str:
    for name in PAT_ENV_VARS:
        value = os.environ.get(name)
        if value:
            return value.strip()
    raise AzureDevOpsError(
        "PAT が見つかりません。次のいずれかの環境変数に設定してください: "
        + ", ".join(PAT_ENV_VARS)
    )


def parse_args(argv: Optional[Sequence[str]] = None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Azure DevOps の作業項目を Loop 貼り付け用の表として出力する",
    )
    parser.add_argument("--org", required=True, help="組織名 (https://dev.azure.com/<org>)")
    parser.add_argument("--project", required=True, help="プロジェクト名")
    parser.add_argument(
        "--team",
        help="チーム名。@CurrentIteration の解決に使う。省略するとプロジェクト既定チーム。",
    )
    parser.add_argument(
        "--iteration-path",
        help="スプリントを直接指定する (例: 'MyProject\\Sprint 12')。指定時は現スプリントの代わりに使う。",
    )
    parser.add_argument(
        "--out-dir", default=".", help="出力先ディレクトリ (既定: カレントディレクトリ)"
    )
    parser.add_argument("--basename", default="azdo-progress", help="出力ファイル名の基底")
    parser.add_argument("--heading", help="表の見出し (既定: プロジェクト名とスプリント名)")
    parser.add_argument(
        "--no-ancestors",
        action="store_true",
        help="スプリント外の親 (Epic / Feature) を取得せず、平坦な一覧にする",
    )
    parser.add_argument(
        "--done-state",
        action="append",
        dest="done_states",
        help="完了とみなす状態。複数回指定できる (既定: {})".format(
            ", ".join(DEFAULT_DONE_STATES)
        ),
    )
    parser.add_argument("--api-version", default=API_VERSION, help="API バージョン")
    return parser.parse_args(argv)


def main(argv: Optional[Sequence[str]] = None) -> int:
    args = parse_args(argv)
    try:
        pat = read_pat()
        client = Client(args.org, args.project, pat, args.api_version)

        wiql = build_wiql(args.project, args.team, args.iteration_path)
        ids = query_ids(client, args.team, wiql)
        if not ids:
            print("該当する作業項目がありませんでした。", file=sys.stderr)
            return 1

        items = fetch_work_items(client, ids)
        known = items if args.no_ancestors else fetch_ancestors(client, items)
        roots = build_forest(known, items.keys(), args.org, args.project)
        rows = flatten(roots)
        scoped = [node for _, node in rows if node.in_scope]

        iterations = {
            node.fields.get("System.IterationPath", "") for node in scoped
        }
        iteration_path = (
            args.iteration_path
            or (next(iter(iterations)) if len(iterations) == 1 else "")
        )
        iteration_label = iteration_path.split("\\")[-1] or "現在のスプリント"
        heading = args.heading or "{} ／ {}".format(args.project, iteration_label)
        subtitle = "取得日時: {}".format(_dt.datetime.now().strftime("%Y-%m-%d %H:%M"))
        done_states = args.done_states or DEFAULT_DONE_STATES

        os.makedirs(args.out_dir, exist_ok=True)
        html_path = os.path.join(args.out_dir, args.basename + ".html")
        md_path = os.path.join(args.out_dir, args.basename + ".md")
        with open(html_path, "w", encoding="utf-8") as fp:
            fp.write(render_html(rows, scoped, heading, subtitle, done_states))
        with open(md_path, "w", encoding="utf-8") as fp:
            fp.write(render_markdown(rows, scoped, heading, subtitle, done_states))

        print("作業項目 {} 件 (親を含めて {} 行) を書き出しました。".format(len(scoped), len(rows)))
        print("  HTML: {}".format(html_path))
        print("  Markdown: {}".format(md_path))
        print("HTML をブラウザで開き、表を選択してコピーすると Loop に表として貼り付けられます。")
        return 0
    except AzureDevOpsError as exc:
        print("エラー: {}".format(exc), file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
