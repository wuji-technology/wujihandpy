#!/usr/bin/env bash
# docs-followup-prompt.sh
#
# Render the docs follow-up Claude prompt for a merged code PR.
# Self-hosted variant for wujihandpy — hardcoded product config (Wuji Hand
# Python SDK / sdk-user-guide section). No dependency on wuji-docs-center
# repos.json.
#
# Required environment variables:
#   REPO         owner/name of the repository
#   PR_NUMBER    The merged code PR number
#   PR_TITLE     The merged code PR title
#   PR_AUTHOR    GitHub login of the merged code PR author

set -euo pipefail

: "${REPO:?REPO env required (owner/name)}"
: "${PR_NUMBER:?PR_NUMBER env required}"
: "${PR_TITLE:?PR_TITLE env required}"
: "${PR_AUTHOR:?PR_AUTHOR env required}"

PRODUCT_NAME="Wuji Hand Python SDK (wujihandpy)"
DOCS_PATH="docs/external"
SURFACE_HINT="Python 公开 API 签名、参数名、返回类型、示例代码、安装/使用步骤"
NOISE_HINT="内部模块拆分、cython 实现细节、CI/构建变更、类型注解微调（无 API 影响时）"
EXTRA_PATHS="\`README.md\`、\`pyproject.toml\`（版本字段）、\`examples/\`"
CHANGELOG_HINT="API 变更必须标明是否破坏兼容；示例代码与 docstring 同步更新"

# Defensive: GitHub PR titles can in principle contain triple backticks, which
# would prematurely close the Feishu fenced code block this prompt renders into.
PR_TITLE_SAFE="${PR_TITLE//\`\`\`/ʼʼʼ}"

cat <<EOF
你是 ${PRODUCT_NAME} 文档维护助手。仓库 ${REPO} 的 PR #${PR_NUMBER}（${PR_TITLE_SAFE}，作者 @${PR_AUTHOR}）已合入 main。

任务：
1. 取代码改动：运行 \`gh pr diff ${PR_NUMBER} -R ${REPO}\`，并读 PR 描述
2. 读取本仓库对外文档目录的现有内容（\`${DOCS_PATH}/\`、${EXTRA_PATHS}）
3. 判断这次改动是否产生用户可感知变化：${SURFACE_HINT}
4. 需要更新 → 修改对应文档文件；文档缺失而变更需要新文档 → 新建
5. 不产生用户可感知变化 → 回复"无需改动"并说明理由

判断规则：
- 只关注用户可感知变更（${SURFACE_HINT}）
- 忽略 ${NOISE_HINT}

对外性判定（判断"用户可感知"的依据，2026-07-09 强化）：
- 依据是本仓对外 API 面（.pyi / 导出类型 / ROS 接口 / 头文件等）里的公开符号 + PR 的 Breaking / 新 API 标记，不是 CHANGELOG 是否已记。changelog 缺失是"应记未记"，不能反推"无需跟进"
- Breaking 变更、或公开符号新增 / 签名变更：即使 changelog 未记、即使当前文档无对应章节，也要跟进（无章节就新建承载），不可判 skip
- 确属对外但 CHANGELOG [Unreleased] 未记 → 在跟进结论里提醒源 PR 作者补记，这是发布记录的唯一取材源，漏记会导致发版漏条目，但缺失本身不作为 skip 理由

版本发布记录（与正文承载独立判定，2026-07-09 起）：
- 本仓 docs/external 不承载 release-notes 页面，版本发布记录由发版流程从本仓 CHANGELOG.md 聚合生成到文档中心对应产品页
- 跟进时确认源 PR 的用户可感知变化已记入 CHANGELOG.md 的 [Unreleased] 段（以 origin/main 为准）：缺失 → 在跟进报告里点名提醒源 PR 作者补记，发布记录以 [Unreleased] 为唯一取材来源，漏记会导致发版漏条目
- 已随既往版本发布的变化无需处理

分支与合入目标（发布列车）：
- 判定需要改文档时，从 \`docs-prerelease\` 拉跟进分支，PR base 也用 \`docs-prerelease\`。线上文档由每日 tag 从各仓 main 构建，直接合 main 会把未发布功能的文档提前带上线。列车分支在双周发布日统一合入 main，main 的日常变更由保鲜 workflow 自动同步进列车分支
- 本仓是公开仓，受组织 ruleset 限制无法向上游推新分支：推到个人 fork，从 fork 向上游提 PR，base 仍是上游 \`docs-prerelease\`
- 涉及 docs-center 侧配套变更（meta.json 导航注册、reading-guide.json）时，同样走 wuji-docs-center 的 \`docs-prerelease\`

写作规范：
- 遵守 Microsoft Writing Style Guide
- 中文：删除"的/了/进行"等冗余词；用阿拉伯数字；主动语态、现在时
- 同一概念始终用同一术语；段落 3-7 行
- 不引入 frontmatter description 字段
- ${CHANGELOG_HINT}
EOF
