# Command console reference

The command console (**View ▸ Command console**, or type anywhere in the main window when the
console has focus) runs the same validated, undoable operations as the ribbon and the dialogs.
A command that cannot run reports why and changes nothing.

## Input conventions

* Command names and keywords are case-insensitive; aliases are listed in the table.
* **Points**: `x,y,z` (absolute), `x,y` (z = the active work plane), `@dx,dy[,dz]` (relative to the
  previous point of the same command), `@distance<bearing` (polar in plan, bearing in degrees
  clockwise from grid north). Example: `POLYLINE -25000,2900000,1200 @-90,0,-11.25 @0,-60,-7.5`.
* **While a viewport tool is running** (e.g. `LINE` without arguments), typed points and numbers
  go to the tool: a point is the next vertex; a number is the tool's value where it has one
  (offset distance, fillet radius, chamfer distance, text height, circle radius, rotation angle,
  scale factor) and otherwise a distance from the last point towards the cursor.
* Commands act on the current **selection** unless they take object names or ids.
* `HELP <command>` prints the usage of one command; `UNDO [n]` / `REDO [n]` step through the undo
  history; long operations (filters, registration, meshing, optimisation) run as background
  jobs — `JOBS` lists them, `JOBS CANCEL` cancels them, `WAIT` blocks until they finish (scripts).
* Plugin commands run with `PLUGIN <command> [JSON arguments]` (see [plugins.md](plugins.md));
  Python statements with `PYTHON <statement>` and script files with `RUNSCRIPT <file.py>`.

## Commands

The table is generated from the application's command registry
(`openmine-desktop --command-reference <file>`); the `console_reference_up_to_date` test fails
when it no longer matches the build. `CRS` exists only in builds with PROJ.

<!-- BEGIN GENERATED: openmine-desktop --command-reference -->
| Command | Usage | Description | Aliases |
|---|---|---|---|
| `ACTIVITY` | `ACTIVITY [rate m/day]` | Schedule activity linked to the selected heading | `ACT` |
| `ACTUAL` | `ACTUAL <quantity> [YYYY-MM-DD] [face chainage]` | Record dated actual progress on the selected activity (or the heading's activity) |  |
| `APPLYDESIGN` | `APPLYDESIGN` | Set flagged (or selected) activity quantities from the current design (audited) |  |
| `ARC` | `ARC [p1 p2 p3]` | Arc through three points (no args: draw in the viewport) | `A` |
| `AREA` | `AREA` | Plan area and perimeter of the selected closed curves (no selection: pick in the viewport) | `AA` |
| `ASSIGN` | `ASSIGN <resource> [units] [DRIVES]` | Assign a resource to the selected activities |  |
| `ATTACH` | `ATTACH <file> [description]` | Attach a copy of a file to the selected object (or the project) |  |
| `BASELINE` | `BASELINE <name>` | Capture an approved, immutable baseline of the active schedule |  |
| `CHAMFER` | `CHAMFER [d1 [d2]]` | Chamfer two curves (pick them in the viewport) | `CHA` |
| `CHECK` | `CHECK` | Check the project file, model references and assets |  |
| `CIRCLE` | `CIRCLE [centre radius]` | Create a circle (no args: draw in the viewport) | `CI` |
| `CLOSE` | `CLOSE` | Close the project |  |
| `COMPACT` | `COMPACT [DRYRUN]` | Remove stored files no project object uses (saves; clears undo) |  |
| `COMPARE` | `COMPARE <reference surface> [maxDistance]` | Distance of the selected surface to a reference surface |  |
| `COPY` | `COPY [dx,dy[,dz]]` | Copy the selection with an offset | `CO`, `CP` |
| `CROP` | `CROP x1,y1,z1 x2,y2,z2 [OUTSIDE] \| CROP (draw a polygon)` | Crop the selected point cloud by a box or a drawn polygon |  |
| `CRS` | `CRS [EPSG:code \| definition]` | Show or declare and set the project CRS (coordinates are never transformed) |  |
| `DECIMATE` | `DECIMATE triangles [maxError]` | Decimate the selected surface |  |
| `DELETE` | `DELETE` | Delete the selection (UNDO restores it) | `DEL`, `E`, `ERASE` |
| `DIMENSION` | `DIMENSION [p1 p2 [p3]]` | Aligned dimension between two points (offset through p3) | `DIM` |
| `DISTANCE` | `DISTANCE [p1 p2]` | Measure between two points (no args: pick in the viewport) | `DI`, `DIST` |
| `ELEV` | `ELEV [z]` | Show or set the work-plane elevation used for drawing | `WORKPLANE` |
| `EXPLODE` | `EXPLODE` | Explode the selected objects into their parts | `X` |
| `EXPORT` | `EXPORT <path>` | Export the selection (or everything applicable) by file type |  |
| `EXTEND` | `EXTEND` | Extend curves to boundaries | `EX` |
| `EXTRUDE` | `EXTRUDE height \| dx,dy,dz [SYM] [NOHOLES]` | Extrude the selected closed curves into solids | `EXT` |
| `FILLET` | `FILLET [radius]` | Fillet two curves (radius 0 = sharp corner) | `F` |
| `FILTER` | `FILTER OUTLIERS [k ratio] \| RADIUS r n \| VOXEL size [CENTROID] \| SPACING d \| RANDOM n \| GROUND  [OPEN3D]` | Filter the selected point cloud into a derived dataset (the original is never changed) |  |
| `GRIDSNAP` | `GRIDSNAP [spacing \| OFF]` | Grid snap with a spacing, or off |  |
| `GRIP` | `GRIP [n x,y,z\|@dx,dy,dz]` | List the grips of the selected object, or move grip n |  |
| `HEADING` | `HEADING <id> [width height] [level code]` | Make the selected centreline a development heading | `DEV` |
| `HEAL` | `HEAL` | Heal the selected solids (shape fixing, orientation, face merging) |  |
| `HELP` | `HELP [command]` | List commands or show help for one command | `?` |
| `ID` | `ID` | Report the coordinates of a picked point |  |
| `IMPACTS` | `IMPACTS` | Headings whose design changed since their schedule quantities were approved |  |
| `IMPORT` | `IMPORT <path>` | Import a file by type (point clouds, DXF, OMF, meshes, STEP/IGES, GIS, DEM, CSV tables, plugin formats) |  |
| `INTERSECT` | `INTERSECT` | Boolean intersection of the selected solids | `IN` |
| `JOBS` | `JOBS [CANCEL]` | List background jobs, or cancel them all |  |
| `JOIN` | `JOIN` | Join the selected curves end to end | `J` |
| `LAYER` | `LAYER <name> [#RRGGBB]` | Make a layer current (created when it does not exist) | `LA` |
| `LAYOUT` | `LAYOUT 1\|2\|2H\|4` | Number of views |  |
| `LENGTH` | `LENGTH` | Length and gradient of the selected curves | `LEN` |
| `LINE` | `LINE [p1 p2]` | Create a line (no args: draw in the viewport) | `L` |
| `LINK` | `LINK <heading id>` | Link the selected activity to a heading |  |
| `LIST` | `LIST` | Summarise the project contents |  |
| `LOFT` | `LOFT [RULED] [SHELL]` | Loft through the selected closed sections (in selection order) |  |
| `MESH` | `MESH DELAUNAY [maxEdge] \| MESH POISSON [depth] [OPEN3D\|PDAL]` | Surface from the selected point cloud |  |
| `MIRROR` | `MIRROR [p1 p2]` | Mirror the selection about the vertical plane through two points (keeps the original) | `MI` |
| `MOVE` | `MOVE [dx,dy[,dz]]` | Move the selection (no args: base and target points in the viewport) | `M` |
| `NEW` | `NEW <name> [YYYY-MM-DD]` | Create a new project (schedule starts today unless a date is given) |  |
| `NORMALS` | `NORMALS [k] [UP\|IN\|OUT] [OPEN3D]` | Estimate normals of the selected point cloud |  |
| `OFFSET` | `OFFSET [distance]` | Offset a curve (pick the object and the side) | `O` |
| `OPEN` | `OPEN <path>` | Open an .openmine project |  |
| `ORTHO` | `ORTHO [ON\|OFF]` | Ortho mode on/off |  |
| `PLUGIN` | `PLUGIN [command [json arguments]]` | Run a plugin command (no args lists them) |  |
| `POINT` | `POINT [p …]` | Create points (no args: pick in the viewport) | `PO` |
| `POLYGON` | `POLYGON [centre radius sides [rotation°]]` | Regular polygon (inscribed); no args: draw a free polygon | `POL` |
| `POLYLINE` | `POLYLINE [p1 p2 … [CLOSE]]` | Create a polyline (no args: draw in the viewport) | `PL`, `PLINE` |
| `PROP` | `PROP [column value…]` | Set a property of the selection (PROP alone lists the editable columns) | `SET` |
| `PYTHON` | `PYTHON <statement>` | Run a Python statement in the embedded interpreter | `PY` |
| `RECTANGLE` | `RECTANGLE [corner1 corner2]` | Create a rectangle on the work plane | `REC`, `RECT` |
| `REDO` | `REDO [n]` | Redo undone change(s) |  |
| `REGISTER` | `REGISTER ICP <target cloud> [maxCorr] [OPEN3D] [APPLY] \| REGISTER PAIRS` | Register the selected point cloud (ICP onto a target, or open point-pair registration) | `REG` |
| `REPAIR` | `REPAIR [fillHolesUpToEdges]` | Repair the selected surface |  |
| `REPORT` | `REPORT <kind> [pdf\|html\|xlsx]` | Generate a report: conformance, registration, pointcloud, gradetonnage, gantt, schedule-xlsx, production, scenarios, quantities |  |
| `RESOURCE` | `RESOURCE <name> [capacity] [EQUIPMENT\|CREW\|MATERIAL\|OTHER]` | Add a resource to the library |  |
| `REVOLVE` | `REVOLVE [angle] [px,py,pz dx,dy,dz]` | Revolve the selected closed curves (axis: a selected line, or the given one) | `REV` |
| `ROTATE` | `ROTATE [degrees [base]]` | Rotate the selection about a vertical axis (counter-clockwise positive) | `RO` |
| `RUNSCRIPT` | `RUNSCRIPT <path.py>` | Run a Python script file |  |
| `SAVE` | `SAVE [path]` | Save the project (atomic, verified) | `SAVEAS` |
| `SCALE` | `SCALE [factor [base]]` | Scale the selection uniformly about a base point (default: the start of the first selected object) | `SC` |
| `SCENARIO` | `SCENARIO [name \| BASE]` | Make a scenario active (no args lists them) |  |
| `SCHEDULE` | `SCHEDULE` | Recalculate the schedule of the active scenario | `CALC`, `CPM` |
| `SCREENSHOT` | `SCREENSHOT <path.png>` | Save the active view as an image |  |
| `SECTION` | `SECTION <heading id> <chainage> \| SECTION OFF` | Vertical section across a heading at a chainage | `SEC` |
| `SECTIONOBJ` | `SECTIONOBJ` | Section the selected solids and meshes by the section plane (or the work plane) |  |
| `SEGMENT` | `SEGMENT tolerance [minSize] [GROW angle] [DROP]` | Cluster the selected point cloud (connected components or region growing) | `CLUSTER` |
| `SELECT` | `SELECT <name \| heading id \| uuid \| ALL \| NONE>…` | Select entities by name, heading ID or id (+name adds) | `SEL` |
| `SMOOTH` | `SMOOTH [iterations]` | Taubin-smooth the selected surface |  |
| `SNAP` | `SNAP [ON\|OFF]` | Object snap on/off |  |
| `SOLIDCHECK` | `SOLIDCHECK` | Validity, volume, area and topology of the selected solids and meshes | `CHECKSOLID` |
| `SOLIDFILLET` | `SOLIDFILLET <radius>` | Round every edge of the selected solids |  |
| `SOLIDOFFSET` | `SOLIDOFFSET <distance>` | Offset the faces of the selected solids (negative: inwards) |  |
| `SPLINE` | `SPLINE [fit points … [CLOSE]]` | Spline through fit points | `SPL` |
| `SPLIT` | `SPLIT` | Split a curve at a point | `BR`, `BREAK` |
| `STATUSDATE` | `STATUSDATE <YYYY-MM-DD [HH:MM] \| NONE>` | Set the data date for reforecasting remaining work |  |
| `SUBTRACT` | `SUBTRACT` | Subtract the later selected solids from the first | `SU` |
| `SWEEP` | `SWEEP width height \| SWEEP PROFILE <name>` | Sweep a tunnel profile along the selected path curves |  |
| `TEXT` | `TEXT [insertion height text…]` | Create a text annotation | `DT` |
| `THEME` | `THEME DARK\|LIGHT` | Switch the colour theme |  |
| `TRIM` | `TRIM` | Trim curves at cutting edges | `TR` |
| `UNDO` | `UNDO [n]` | Undo the last change(s) | `U` |
| `UNION` | `UNION` | Boolean union of the selected solids | `UNI` |
| `VIEW` | `VIEW TOP\|NORTH\|EAST\|SOUTH\|WEST\|ISO\|ISO-NW` | Set a standard view direction | `V` |
| `VOLUME` | `VOLUME` | Design quantities of the selected headings | `VOL` |
| `WAIT` | `WAIT [seconds]` | Wait for the background jobs to finish (scripts) |  |
| `ZOOM` | `ZOOM [EXTENTS \| SELECTION \| factor]` | Zoom the active view | `FIT`, `Z`, `ZE` |
<!-- END GENERATED -->
