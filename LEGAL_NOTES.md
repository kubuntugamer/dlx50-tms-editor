# Legal Notes — Accompanies LEGAL_BRIEF.md

This page records facts that came after the original development of the
project. It does not change the brief. Read it together with
[LEGAL_BRIEF.md](LEGAL_BRIEF.md).

> This is not legal advice. Counsel should review it before it is relied upon.

---

## Entry 1 — Later consultation of the DL1030 owner's manual

**Date of entry:** ____________ (fill in the date the manual was consulted)

**Timeline.** The original development of this project used only the author's
own `.TMS` data files and the author's own observations. The author had not
configured the scale for about eight years. Later, while testing the scale at
work, the author consulted the DL1030 owner's manual (Rev C,
8529-0637-2M) to recall how to retrieve the `.TMS` files from the scale. That
was the only thing the author recalled needing from the manual.

**Separate review by the coding assistant.** While checking the PLU tab's
columns, the assistant read the manual's Fast PLU Programming section
(pages 19-20) and compared the step order with the reference file's PLU rows.
This was an analysis step only. The PLU column labels were not changed.

**What was not taken from the manual.**
- No manual text was copied into the code, comments, or repository.
- The PLU labels in the tab were not changed based on the manual.

**What was not used.**
- No screenshots of the vendor software were obtained or used.
- No vendor software was obtained, installed, or examined.

**Code changes made after the consultation.**
- `tmseditorwindow.cpp`: two column headers changed so they match the values
  in the reference file. The CLS second column is now "Name". The INF columns
  are now "Store name" and "Location". The headers were chosen from the
  file's data, not from the manual.
- The PLU column labels were not changed.

**Open question for counsel.** Whether consulting the manual, even without
copying its text, affects the independence analysis in Section XI of
LEGAL_BRIEF.md, and whether any future PLU labels taken from the manual would
need review first.
