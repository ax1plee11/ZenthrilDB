# Dissertation audit

## Verification performed

- `Zenthril_Dissertation.docx` was reopened with `python-docx` after generation.
- The document contains hierarchical headings, a Word TOC field, tables and seven embedded diagrams.
- Every performance-related section uses “не измерено” where no benchmark was run.
- The text describes recovery, transaction and WAL components as foundations where the source lacks full behavior.
- The text explicitly excludes unprovided Zenthril components and unimplemented encryption.

## Manual completion required

1. Fill in institution, department, author, supervisor, city and local formatting requirements.
2. Open the document in Word and update TOC with F9; Word will paginate based on the installed fonts and printer settings.
3. Conduct dedicated benchmarks, sanitizer runs and lock/deadlock tests before adding quantitative claims.
4. Add Zenthril materials only after a separate source audit.
