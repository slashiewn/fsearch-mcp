# fsearch-mcp

FSearch 引擎驱动的 MCP Server——给 Claude Code 提供毫秒级文件搜索能力。

## 快速开始

```bash
# 构建
make

# Claude Code 注册 (添加到 .claude/settings.local.json)
```

## MCP 配置

添加到你的 `.claude/settings.local.json`:

```json
{
  "mcpServers": {
    "fsearch": {
      "command": "/home/wei/projects/fsearch-mcp/fsearch-mcp",
      "args": ["--db", "/home/wei/.fsearch-mcp/index.db", "--dir", "/home/wei"]
    }
  }
}
```

## 提供的工具

| 工具 | 功能 |
|------|------|
| `search` | 搜索已索引文件（glob/部分名/regex） |
| `scan` | 扫描目录构建索引 |
| `save` | 持久化索引到磁盘 |
| `load` | 从磁盘加载索引 |
| `status` | 数据库状态 |

## 许可证

GPL v2（继承自 FSearch）
