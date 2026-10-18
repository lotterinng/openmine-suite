# Python plugin sample — `openmine-levels-sample`

Commands:

| Command | Arguments (JSON) | Result |
|---|---|---|
| `levels.summary` | — | headings, design metres and volume per development level |
| `levels.layers` | `{"prefix": "Level ", "color": 8369109}` | creates one layer per level (one undo step) |

Install it into the Python environment used by OpenMine (`pip install .`), or copy the
`openmine_levels` package together with a `*.dist-info` folder into the `python` subfolder of a
plugin folder — the build does exactly that into `<build>/plugins/python`, which is how the GUI
test loads it. The application lists the commands in **Tools ▸ Plugins** and runs them with
`PLUGIN levels.summary` in the command console.
