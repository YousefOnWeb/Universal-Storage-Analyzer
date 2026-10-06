# Module boundaries

| Path | May use | Must not |
| --- | --- | --- |
| `src/domain` | C++20 stdlib only | Qt of any kind |
| `src/services` | QtCore, QtSql, stdlib, `domain` | QtWidgets |
| `src/services/scanner` | C++20 stdlib + OS platform APIs only | Qt of any kind |
| `src/models` | QtCore/Gui item models, `services` types | filesystem, SQL, widgets |
| `src/ui` | QtWidgets, `models`, `services` interfaces | direct filesystem/SQL scanning |
| `src/app` | everything (wiring only) | business logic |

Enforcement: code review + `grep -rn "QtWidgets\|QWidget" src/domain src/services`
must return nothing (scanner additionally: no `#include <Q` at all).
