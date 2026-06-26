#pragma once

// Periodic connected-time accounting. Once started, a background task credits
// online visitors' time budgets, enforces the configured cap (cutting off
// over-budget visitors), and flushes the persistent visitor table. Portal mode
// only — start it after the client table and authz are up.
void accounting_start(void);
