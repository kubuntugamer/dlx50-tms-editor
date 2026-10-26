# Legal Brief — Independent Development & Non-Infringement of *TMS / DLX‑50‑style Editor*

**Prepared:** October 25, 2026
**Purpose:** Record of the legal basis on which this open-source project is
built, prepared in anticipation of a possible cease-and-desist demand or
infringement claim asserted by the owner of the DETECTO / DLX‑50 brand, and
published in this repository as notice of the project's independent,
from-scratch provenance.

> **This is not legal advice.** It was prepared by a non-lawyer with AI
> assistance, states U.S. law as of the date above, and should be reviewed by
> licensed counsel before being relied upon in any actual dispute. Nothing
> here creates an attorney-client relationship.
>
> **Companion file:** [CITATIONS.md](CITATIONS.md) — every statute and case
> cited below, linked to free full-text sources (all links verified
> October 2026).

---

## I. Question Presented

Does an independently written Qt 6 desktop application that reads and writes
plain-text `.TMS` data files (the format used by DLX‑50-style retail scales)
expose its author to liability under U.S. copyright, DMCA anti-circumvention,
trade secret, trademark, or patent law — where:

1. the file format is open and human-readable (plain ASCII, no encryption, no
   access control, no binary encoding);
2. the vendor's original software was **never possessed, installed, or
   examined** in any form (distribution media long lost; download gated behind
   credentials no longer available); and
3. the implementation was developed from scratch by observing the structure of
   the author's own data files and publicly available specifications?

## II. Summary of Conclusions

| Theory | Exposure | Core reason |
|---|---|---|
| Copyright infringement | **None** | No copying-in-fact (independent creation, zero access); a file format is an unprotectable §102(b) system/method of operation; file contents are facts |
| DMCA §1201 | **None** | No technological protection measure exists or was circumvented — the format is plain ASCII |
| Trade secret (DTSA/UTSA) | **None** | No improper means, no confidentiality duty, no secrecy measures; knowledge came from open files and public specs |
| Trademark / false designation | **Low** (nuisance-level) | Nominative + classic fair use, disclaimer, no logos; worst realistic case is a rename |
| Patent | **None** | Distributing a file editor neither makes, uses, nor sells any claimed scale apparatus or method |
| State unfair competition / misc. | **Rises & falls with the above** | Largely preempted (17 U.S.C. §301) or fails with the Lanham analysis |

## III. Material Facts (documented in this repository's history)

1. **The format is open text.** `.TMS` files are ASCII with CRLF line
   terminators and tab-delimited fields (verified: `file A_000.TMS` →
   *"ASCII text, with CRLF line terminators"*). They contain labeled,
   human-readable sections — e.g. `DWL … END` blocks holding department and
   class records with plain-English descriptors such as "Service Fee" and
   "Discount & Rounding", plus `ECS VER` headers. The entire file can be read
   with `cat`. **No encryption, no compression, no obfuscation, no access
   control of any kind.**
2. **No original software was ever in the picture.** The DLX‑50 distribution
   CD-ROM was lost years ago; the vendor's download portal remains gated
   behind credentials long since unavailable. Nothing was decompiled,
   disassembled, debugged, or copied — there was no original program present
   to do any of that to.
3. **Development method.** The parser, model, and UI were written from
   scratch in C++17 / Qt 6 solely from the observable structure of the
   author's own data files and publicly available specification material. No
   code was translated or adapted. The git history records the incremental,
   independent evolution of the project.
4. **Naming.** The project is titled "TMS / DLX‑50‑style Editor" — vendor and
   model names appear only descriptively, in plain text, with an explicit
   affiliation disclaimer in the README.
5. **Distribution.** MIT-licensed, offered at no charge, full source
   available. Non-commercial in the ordinary sense.

## IV. Copyright Law

### A. The threshold element — copying-in-fact — fails outright

Copyright infringement requires **copying of protectable elements**. Independent
creation is a complete defense: *Feist Publ'ns, Inc. v. Rural Tel. Serv. Co.*,
499 U.S. 340 (1991) (originality — and thus independent creation of it — is
the *sine qua non* of copyright; no matter how similar, independently created
works do not infringe). Here the author had **no access whatsoever** to any
vendor program (Fact 2). With access absent, no factfinder could conclude
copying occurred; and any residual similarity between two independently
written programs would consist entirely of functional features, which are
unprotectable for the independent reasons below. Absent copying, the analysis
never even reaches substantial similarity.

### B. The file format itself is an unprotectable system / method of operation

- **17 U.S.C. §102(b):** copyright protection "does not extend to any idea,
  procedure, process, system, [or] method of operation." A file layout that a
  program must emit and another must parse is exactly such a system.
- ***Baker v. Selden*, 101 U.S. 99 (1879):** one may freely use the
  *system* described in another's work; copyright is not a patent. The
  analogy here is direct: a specification of how records are arranged on disk
  is the "system," and users of the format may implement it.
- ***Lotus Dev. Corp. v. Borland Int'l, Inc.*, 49 F.3d 807 (1st Cir. 1995),
  aff'd, 516 U.S. 696 (1996):** even a program's entire command-word
  hierarchy was an unprotectable "method of operation." A fortiori, a data
  record structure whose only purpose is to be operated on by programs.
- ***Google LLC v. Oracle Am., Inc.*, 593 U.S. 1 (2021):** the Supreme Court's
  most recent software-copyright decision reaffirmed that achieving
  interoperability through use of declared structure is the paradigm of
  permissible reuse, resolving the case in the implementer's favor on fair
  use even assuming copyrightability.
- **Merger:** to the extent particular fields and markers are dictated by the
  scale firmware that must parse them, expression merges with idea and
  receives, at most, "thin" protection — *Herbert Rosenthal Jewelry Corp. v.
  Kalpakian*, 446 F.2d 738 (9th Cir. 1971) (idea-expression merger); *Computer
  Assocs. Int'l, Inc. v. Altai, Inc.*, 982 F.2d 693 (2d Cir. 1992) (the
  abstraction-filtration-comparison test filters out elements dictated by
  efficiency, external constraints, and industry standards before any
  comparison).
- **Scènes à faire:** records and field names that are stock to the retail
  weighing-scale domain — tare weight, PLU/price-look-up codes, department
  and class tables, reserved placeholder entries — are unprotectable as
  elements common to the idea; *Morrissey v. Procter & Gamble Co.*, 379 F.2d
  675 (1st Cir. 1967) (standardized elements of a genre not protected).

### C. The file's contents are facts

*Feist*, 499 U.S. 340: facts are not copyrightable. The numbers and names
inside a store's `.TMS` file — PLU codes, product names, weights, prices —
are facts about that store's inventory. Reading, editing, and rewriting them
is infringement of nothing, regardless of how the file is laid out.

### D. No literal copying

The application is original C++/Qt source. It contains no line of vendor
code, because no vendor code was ever available to it. Direct (literal)
infringement is therefore impossible; only non-literal theories remain, and
Sections IV.B–C dispose of them.

### E. Fair use as an independent fallback (17 U.S.C. §107)

Assume *arguendo* some structural element of the format were protectable:

- ***Sega Enters. Ltd. v. Accolade, Inc.*, 977 F.2d 1510 (9th Cir. 1992):**
  copying undertaken to achieve interoperability between independent programs
  is quintessential fair use — the purpose is transformative and favored.
- ***Sony Computer Entertainment, Inc. v. Connectix Corp.*, 203 F.3d 596 (9th
  Cir. 2000):** intermediate copying during reverse engineering for
  interoperability is fair use.
- ***Google v. Oracle*, 593 U.S. 1 (2021):** using structure to reach a new
  platform and new audience is transformative; market harm must be shown for
  the specific market at issue.

Applied here: **purpose** — enabling owners of scales and of data files to
read and edit their own files (transformative, freely distributed at no
charge); **nature** — factual and functional; **amount** — only the structural
information necessary for input/output, never literal code; **effect** — the
only conceivable displaced market would be a market for the vendor's own
editing software, which is not offered to this author. This fourth factor is
the one a plaintiff would contest; it is precisely the factor on which
*Sega*, *Connectix*, and *Google* nonetheless ruled for the interoperability
tool.

### F. Replicating observable behavior is not copying

The save routine (write `<file>.BAK`, then overwrite the original) reproduces
the *observable behavior* of the original tool. Behavior is a method of
operation, not expression — §102(b); *Lotus*, 49 F.3d 807. Implementing it is
the essence of compatibility, not infringement.

## V. DMCA Anti-Circumvention — 17 U.S.C. §1201

Sections 1201(a) and (b) apply only where a **technological measure
effectively controls access** to, or protects, a copyrighted work, and that
measure is **circumvented**. Here there is no such measure: the files are
plain ASCII (Fact 1) — no encryption, no authentication, no scrambling, no
lock. There is nothing to circumvent, and nothing was. No §1201 claim can be
stated against this project on these facts. Independently, §1201(f) expressly
authorizes circumvention necessary to achieve **interoperability** of
independent programs, and permits reverse engineering for that purpose —
neither was even needed, but both foreclose the theory.

## VI. Trade Secrets — Defend Trade Secrets Act (18 U.S.C. §§1836, 1839) & Uniform Trade Secrets Act

Misappropriation requires acquisition by **improper means** or breach of a
duty of confidence, and "improper means" excludes reverse engineering and
derivation from lawfully held items (18 U.S.C. §1839(5)–(6); UTSA §1(2)
(recognizing derivation and reverse engineering as *proper* means)).

- **No improper means.** Knowledge of the format came from (i) files produced
  by the author's own equipment in ordinary use and (ii) publicly available
  specifications. No vendor premises, systems, personnel, or documents were
  involved.
- **No duty.** No NDA, contract, license, or other relationship with the
  vendor existed under which information was received.
- **No secrecy.** A format printed as plain English text into every ordinary
  customer's data files, with no confidentiality restriction attached, does
  not satisfy the statutory requirement of "reasonable measures … to maintain
  its secrecy" (§1839(3)(A)). The knowledge needed to write this editor was
  available, as a practical matter, to anyone who opened one of these files
  in a text editor.

Reverse engineering was never performed — but had it been, the statute
expressly permits it. The theory fails at every link of the chain.

## VII. Trademark Law (Lanham Act)

Potential claims: §1114(1) (infringement of a registered mark), §1125(a)(1)
(false designation of origin / false association), §1125(c) (dilution).

- **Likelihood of confusion** is the touchstone of §§1114 and 1125(a). The
  use here is **nominative**: the tool must name the product line it
  handles to say what it is. Under *New Kids on the Block v. News Am.
  Publ'g, Inc.*, 971 F.2d 302 (9th Cir. 1992), and *Toyota Motor Sales,
  U.S.A., Inc. v. Tabari*, 610 F.3d 1171 (9th Cir. 2010), such use is
  permitted where (1) the product is not readily identifiable without the
  mark — "DLX‑50-style scale files" cannot be described otherwise; (2) only
  so much of the mark as necessary is used — plain text only; and (3) nothing
  suggests sponsorship or endorsement — the README says in terms that the
  project is *not affiliated with, endorsed by, or connected to* the vendor.
- **Classic fair use, 15 U.S.C. §1115(b)(4):** use of a model designation
  descriptively and other than as a mark, to state compatibility.
- **Model designations** ("DLX‑50") identify a product line and are
  typically weak marks standing alone; the project uses them to point at the
  target product, not as its own source identifier — the project's own name
  leads with the file extension, "TMS."
- ***Dastar Corp. v. Twentieth Century Fox Film Corp.*, 539 U.S. 23 (2003):**
  no "passing off" — the origin of this software (written by its author,
  MIT-licensed, source published) is fully disclosed.
- **Dilution (§1125(c))** requires a famous mark and excludes
  **noncommercial use** (§1125(c)(3)(A)) — a free, interoperability tool
  distributed at no charge falls within the exclusion.
- **Honest assessment:** trademark owners are obliged to police their marks,
  so a polite cease-and-desist letter is the *most likely* form of adverse
  contact of any theory in this brief — not because the use is unlawful, but
  because sending letters is how such marks are maintained. The realistic
  worst case is a **rename**; GitHub keeps old repository URLs redirecting,
  so the cost would be near zero and no code would change.

## VIII. Patent Law (35 U.S.C. §271)

Infringement requires making, using, selling, offering to sell, or importing
the patented invention. A desktop program that edits data files practices no
claim of a weighing-scale patent: it makes no scale, uses no scale, and
reading or writing a file is not the use of a patented apparatus or method of
weighing. This brief makes only a general observation; if a specific patent
were ever asserted, counsel should perform a claim-by-claim comparison — the
general rule is that interoperable file-handling software does not embody the
mechanical and metrological claims such patents contain.

## IX. Other Theories

- **State unfair competition / passing-off claims** are preempted to the
  extent they assert rights equivalent to copyright (17 U.S.C. §301), and
  otherwise fail with the Lanham Act analysis above.
- **Computer Fraud and Abuse Act (18 U.S.C. §1030):** no unauthorized access
  to any protected computer occurred; the files were read on the owner's own
  machines.
- **Contract-based theories:** none — no license, terms of service, or NDA
  with the vendor was entered into by the author.

## X. Exposure Summary

| Theory | Viable-claim likelihood | Worst realistic outcome |
|---|---|---|
| Copyright | Very low | Injunction sought against a tool that copies nothing |
| DMCA §1201 | Effectively zero | — |
| Trade secret | Very low | — |
| Trademark demand letter | Low–moderate (marks get policed) | Rename; URL redirects; no code change |
| Patent | Effectively zero absent an asserted claim | Counsel claim review |
| State-law claims | Low | Largely preempted |

## XI. Evidence of Independence (preserved by this repository)

1. **Git history** — continuous, public record of development from an empty
   repository: format observations → parser → GUI → packaging. No imported
   source, ever.
2. **Specimens are the author's own data**, and are excluded from the
   repository (`.gitignore`) — nothing derived from the vendor is distributed.
3. **README disclaimer** + nominative naming + MIT license + this brief.
4. **Documented absence of the original software** (lost media, gated
   distribution, expired credentials) — the strongest possible answer to any
   access-and-copying allegation: there was nothing to copy from.

## XII. If a Demand Letter Arrives

1. **Do not reply substantively yourself.** Even friendly replies are used;
   the deadlines in such letters are almost never real. Do not contact the
   sender's counsel pro se.
2. **Preserve everything** — no deletions of code, history, or
   correspondence.
3. **Engage IP counsel** and hand over this brief as the starting point.
4. Expect one of three outcomes: (a) the brief + disclaimer closes it;
   (b) a trademark objection → rename (cheap, GitHub redirects); (c)
   anything more serious → counsel decides, including whether a
   declaratory-judgment posture (28 U.S.C. §2201) is appropriate if a genuine
   threat exists.

## XIII. Scope & Limitations

- **Not legal advice** (see header). U.S. federal law as of 2026‑10‑25; state
  law varies; choice of law and venue would depend on the claimant's domicile
  and forum, which this brief does not analyze.
- **Outside the EU/UK**, interoperability of lawfully accessed programs is
  also statutorily blessed (e.g., Directive 2009/24/EC, art. 6 (decompilation
  for interoperability)); local counsel should confirm the details.
- **Employment-side questions** — ownership as between the author and the
  author's employer, or interests in the data files themselves — are outside
  this brief, which addresses third-party vendor claims only.
- **Update trigger:** if any vendor-originated material is ever incorporated
  (code, documentation, screenshots), or if the format is found to involve
  any access-control mechanism, this analysis must be redone by counsel.
