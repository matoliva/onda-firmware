# Decisions

This directory contains durable records for completed and validated Onda
features. Decisions explain what was built, why it was chosen, the evidence that
proved it, and the consequences for later work.

## Lifecycle

1. Proposed or active work lives in `plans/NNN-feature-name.md`.
2. Implementation and required validation satisfy the feature's definition of
   done.
3. The plan is rewritten as `decisions/NNN-feature-name.md`.
4. The completed plan is removed in the same change.

Preserve the feature number and slug during migration. Decisions are historical
records and should only change to correct evidence or record a deliberate change
to the accepted outcome.
