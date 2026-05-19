# Model Execution & Data Safety Constraints

To prevent heap allocation failures (OOM crashes) and unreadable terminal flooding, the following rules **MUST** be strictly observed when interacting with large reverse-engineering databases (Ghidra, IDA, Binary Ninja).

## 1. Strict Pagination and Limits
*   **Default Truncation:** Never attempt to list all elements of a bulk database array simultaneously (e.g., functions, strings, cross-references).
*   **Enforced Cap:** All listing commands (`list_functions`, `list_exports`, `list_imports`) must default to a maximum limit of **50 items per request** unless explicitly overridden by an intentional user prompt.
*   **Offset Strategy:** Use `limit` and `offset` structures natively for multi-page data exploration.
    *   *Correct:* `ghidra - list_functions (limit: 50, offset: 0)`
    *   *Incorrect:* Pulling raw unrestricted database streams.

## 2. Targeted Search Over Bulk Dumping
*   **Filter First:** Instead of fetching lists to search locally, always prefer targeted server-side queries using string filters, regex patterns, or specific memory offsets.
*   **Symbol Resolution:** When searching for an imported API (like `CreateMutexA`), query the Import/External table directly rather than scanning the entire global function map.

## 3. Graceful Interruption Rules
*   **Payload Size Verification:** Before rendering or processing a JSON payload, check the object or array length.
*   **Size Cap:** If an un-paginated array exceeds **1,000 nodes**, halt execution immediately and return a warning: 
    > "Output suppressed: Payload size too large to process safely without pagination."
