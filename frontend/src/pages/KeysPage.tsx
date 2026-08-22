// =============================================================================
// KeysPage — Stage 16: Full Key Management UI
//
// Features:
//   - Browse keys with real ForgeKV data
//   - Search / filter by prefix
//   - Paginate (backend pagination via limit/offset)
//   - View key detail (full value, TTL)
//   - Create key (PUT /key/:key)
//   - Edit key value + TTL (PUT /key/:key)
//   - Delete key with confirmation (DELETE /key/:key)
//   - URL state: ?prefix=...&page=...
//   - Loading + error + empty states
//   - Toast success feedback
//   - Responsive table with horizontal scroll on mobile
//   - Accessible: labels, roles, keyboard-navigable modal
// =============================================================================

import {
  useCallback,
  useEffect,
  useRef,
  useState,
} from "react";
import { useSearchParams } from "react-router-dom";
import { Empty } from "../components/Empty";
import { ErrorMessage } from "../components/ErrorMessage";
import { Loading } from "../components/Loading";
import { Modal } from "../components/Modal";
import { Toast, useToast } from "../components/Toast";
import { deleteKey, getKey, listKeys, setKey } from "../services/api";
import type { KeyInfo } from "../types/api";
import { formatTtl, truncateValue } from "../utils/format";
import pageStyles from "./Page.module.css";
import styles from "./KeysPage.module.css";

// ---------------------------------------------------------------------------
// Constants
// ---------------------------------------------------------------------------
const PAGE_SIZE = 50;

// ---------------------------------------------------------------------------
// TTL form helpers
// ---------------------------------------------------------------------------
type TtlMode = "permanent" | "custom";

function parseTtlFromForm(mode: TtlMode, raw: string): number | undefined {
  if (mode === "permanent") return undefined; // omit header → permanent
  const v = parseFloat(raw);
  return isNaN(v) || v <= 0 ? undefined : v;
}

// ---------------------------------------------------------------------------
// KeysPage
// ---------------------------------------------------------------------------
export function KeysPage() {
  const [searchParams, setSearchParams] = useSearchParams();

  // Derive prefix and page from URL query params
  const urlPrefix = searchParams.get("prefix") ?? "";
  const urlPage = Math.max(1, parseInt(searchParams.get("page") ?? "1", 10) || 1);

  // Local UI state
  const [prefix, setPrefix]       = useState(urlPrefix);
  const [page, setPage]           = useState(urlPage);
  const [keys, setKeys]           = useState<KeyInfo[]>([]);
  const [total, setTotal]         = useState(0);
  const [loading, setLoading]     = useState(true);
  const [refreshing, setRefreshing] = useState(false);
  const [error, setError]         = useState<string | null>(null);
  const hasLoadedRef              = useRef(false);

  // Modal states
  const [viewKey, setViewKey]     = useState<KeyInfo | null>(null);
  const [editKey, setEditKey]     = useState<KeyInfo | null>(null);
  const [deleteTarget, setDeleteTarget] = useState<KeyInfo | null>(null);
  const [createOpen, setCreateOpen] = useState(false);

  const { toasts, addToast, dismissToast } = useToast();

  // ---------------------------------------------------------------------------
  // Sync URL ↔ state
  // ---------------------------------------------------------------------------
  // When URL changes externally (browser back/forward), sync to state.
  useEffect(() => {
    const p = searchParams.get("prefix") ?? "";
    const pg = Math.max(1, parseInt(searchParams.get("page") ?? "1", 10) || 1);
    setPrefix(p);
    setPage(pg);
  // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [searchParams]);

  // ---------------------------------------------------------------------------
  // Fetch keys
  // ---------------------------------------------------------------------------
  const fetchKeys = useCallback(
    async (opts: { prefix: string; page: number; isRefresh?: boolean }) => {
      const { prefix: pfx, page: pg, isRefresh = false } = opts;

      if (hasLoadedRef.current || isRefresh) {
        setRefreshing(true);
      } else {
        setLoading(true);
      }
      setError(null);

      const offset = (pg - 1) * PAGE_SIZE;
      const result = await listKeys({ prefix: pfx, limit: PAGE_SIZE, offset });

      setRefreshing(false);
      setLoading(false);

      if (!result.ok) {
        setError(result.error);
        return;
      }

      hasLoadedRef.current = true;
      setKeys(result.data.keys);
      setTotal(result.data.total);
    },
    []
  );

  // Fetch on mount and whenever prefix/page changes.
  useEffect(() => {
    fetchKeys({ prefix, page });
  // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [prefix, page]);

  // ---------------------------------------------------------------------------
  // Search submit
  // ---------------------------------------------------------------------------
  const [searchInput, setSearchInput] = useState(urlPrefix);

  const handleSearch = useCallback(
    (e: React.FormEvent) => {
      e.preventDefault();
      const trimmed = searchInput.trim();
      setSearchParams(
        trimmed ? { prefix: trimmed, page: "1" } : { page: "1" }
      );
    },
    [searchInput, setSearchParams]
  );

  const handleClearSearch = useCallback(() => {
    setSearchInput("");
    setSearchParams({});
  }, [setSearchParams]);

  // ---------------------------------------------------------------------------
  // Pagination
  // ---------------------------------------------------------------------------
  const totalPages = Math.max(1, Math.ceil(total / PAGE_SIZE));

  const goToPage = useCallback(
    (pg: number) => {
      const next = Math.min(Math.max(1, pg), totalPages);
      const params: Record<string, string> = { page: String(next) };
      if (prefix) params.prefix = prefix;
      setSearchParams(params);
    },
    [prefix, setSearchParams, totalPages]
  );

  // ---------------------------------------------------------------------------
  // Refresh
  // ---------------------------------------------------------------------------
  const handleRefresh = useCallback(() => {
    fetchKeys({ prefix, page, isRefresh: true });
  }, [fetchKeys, prefix, page]);

  // ---------------------------------------------------------------------------
  // Delete
  // ---------------------------------------------------------------------------
  const [deleting, setDeleting] = useState(false);

  const handleConfirmDelete = useCallback(async () => {
    if (!deleteTarget) return;
    setDeleting(true);
    const result = await deleteKey(deleteTarget.key);
    setDeleting(false);
    setDeleteTarget(null);

    if (!result.ok && result.status !== 404) {
      addToast(`Failed to delete "${deleteTarget.key}": ${result.error}`, "error");
      return;
    }
    addToast(`Deleted "${deleteTarget.key}"`, "success");
    fetchKeys({ prefix, page, isRefresh: true });
  }, [deleteTarget, addToast, fetchKeys, prefix, page]);

  // ---------------------------------------------------------------------------
  // Create / edit success callback
  // ---------------------------------------------------------------------------
  const handleMutateSuccess = useCallback(
    (verb: string, key: string) => {
      addToast(`${verb} "${key}"`, "success");
      fetchKeys({ prefix, page, isRefresh: true });
    },
    [addToast, fetchKeys, prefix, page]
  );

  // ---------------------------------------------------------------------------
  // Render helpers
  // ---------------------------------------------------------------------------
  const offset = (page - 1) * PAGE_SIZE;
  const pageFirst = total === 0 ? 0 : offset + 1;
  const pageLast = Math.min(offset + PAGE_SIZE, total);

  // ---------------------------------------------------------------------------
  // Render
  // ---------------------------------------------------------------------------
  return (
    <div className={pageStyles.page}>
      {/* ---- Header ---- */}
      <header className={pageStyles.pageHeader}>
        <div className={styles.headerRow}>
          <div>
            <h1 className={pageStyles.pageTitle}>Keys</h1>
            <p className={pageStyles.pageDescription}>
              Browse, create, update, and delete keys in the ForgeKV store.
            </p>
          </div>
          <div className={styles.headerActions}>
            <button
              className={styles.btnSecondary}
              type="button"
              onClick={handleRefresh}
              disabled={loading || refreshing}
              aria-label="Refresh key list"
            >
              <span
                className={refreshing ? styles.btnIconSpin : styles.btnIcon}
                aria-hidden="true"
              >
                ↻
              </span>
              {refreshing ? "Refreshing…" : "Refresh"}
            </button>
            <button
              className={styles.btnPrimary}
              type="button"
              onClick={() => setCreateOpen(true)}
              aria-label="Create a new key"
            >
              <span aria-hidden="true">+</span> Create Key
            </button>
          </div>
        </div>
      </header>

      {/* ---- Page body ---- */}
      <div className={pageStyles.pageBody}>
        {/* ---- Toolbar: prefix search ---- */}
        <form
          className={styles.toolbar}
          onSubmit={handleSearch}
          role="search"
          aria-label="Search keys by prefix"
        >
          <div className={styles.searchWrap}>
            <span className={styles.searchIcon} aria-hidden="true">⌕</span>
            <input
              className={styles.searchInput}
              type="search"
              value={searchInput}
              onChange={(e) => setSearchInput(e.target.value)}
              placeholder="Filter by prefix, e.g. user:"
              aria-label="Key prefix filter"
              autoComplete="off"
              spellCheck={false}
            />
          </div>
          <button type="submit" className={styles.btnSecondary}>
            Search
          </button>
          {prefix && (
            <button
              type="button"
              className={styles.btnSecondary}
              onClick={handleClearSearch}
              aria-label="Clear prefix filter"
            >
              Clear
            </button>
          )}
        </form>

        {/* ---- Initial loading ---- */}
        {loading && (
          <div style={{ display: "flex", justifyContent: "center", padding: "4rem 0" }}>
            <Loading label="Loading keys…" size="lg" />
          </div>
        )}

        {/* ---- Error ---- */}
        {!loading && error && (
          <ErrorMessage
            title="Could not load keys"
            message={error}
            onRetry={handleRefresh}
          />
        )}

        {/* ---- Loaded state ---- */}
        {!loading && !error && (
          <>
            {/* ---- Empty states ---- */}
            {total === 0 && prefix === "" && (
              <Empty
                icon="⊟"
                title="No keys stored yet"
                description="Create your first key to get started."
                action={
                  <button
                    className={styles.btnPrimary}
                    type="button"
                    onClick={() => setCreateOpen(true)}
                  >
                    + Create Key
                  </button>
                }
              />
            )}

            {total === 0 && prefix !== "" && (
              <Empty
                icon="⊘"
                title={`No keys match "${prefix}"`}
                description="Try a different prefix or clear the search."
                action={
                  <button
                    className={styles.btnSecondary}
                    type="button"
                    onClick={handleClearSearch}
                  >
                    Clear search
                  </button>
                }
              />
            )}

            {/* ---- Table ---- */}
            {total > 0 && (
              <div className={styles.tableContainer}>
                {/* Subtle overlay while background-refreshing */}
                {refreshing && (
                  <div className={styles.tableOverlay} aria-hidden="true">
                    <Loading label="Refreshing…" size="sm" />
                  </div>
                )}

                <div className={styles.tableWrap}>
                  <table
                    className={styles.table}
                    aria-label="Key list"
                    aria-rowcount={total}
                  >
                    <thead>
                      <tr>
                        <th className={styles.colKey} scope="col">Key</th>
                        <th className={styles.colValue} scope="col">Value</th>
                        <th className={styles.colTtl} scope="col">TTL</th>
                        <th className={styles.colActions} scope="col">Actions</th>
                      </tr>
                    </thead>
                    <tbody>
                      {keys.map((ki) => (
                        <KeyRow
                          key={ki.key}
                          item={ki}
                          onView={() => setViewKey(ki)}
                          onEdit={() => setEditKey(ki)}
                          onDelete={() => setDeleteTarget(ki)}
                        />
                      ))}
                    </tbody>
                  </table>
                </div>

                {/* ---- Pagination ---- */}
                <div className={styles.pagination}>
                  <span className={styles.paginationInfo}>
                    {pageFirst}–{pageLast} of {total} key{total !== 1 ? "s" : ""}
                    {prefix ? ` matching "${prefix}"` : ""}
                  </span>
                  <div className={styles.paginationControls}>
                    <button
                      className={styles.btnSecondary}
                      type="button"
                      onClick={() => goToPage(page - 1)}
                      disabled={page <= 1}
                      aria-label="Previous page"
                    >
                      ← Prev
                    </button>
                    <span className={styles.pageLabel}>
                      Page {page} / {totalPages}
                    </span>
                    <button
                      className={styles.btnSecondary}
                      type="button"
                      onClick={() => goToPage(page + 1)}
                      disabled={page >= totalPages}
                      aria-label="Next page"
                    >
                      Next →
                    </button>
                  </div>
                </div>
              </div>
            )}
          </>
        )}
      </div>

      {/* ================================================================== */}
      {/* Modals                                                              */}
      {/* ================================================================== */}

      {/* ---- View key detail ---- */}
      <ViewModal
        item={viewKey}
        onClose={() => setViewKey(null)}
        onEdit={(ki) => { setViewKey(null); setEditKey(ki); }}
      />

      {/* ---- Create key ---- */}
      <CreateModal
        isOpen={createOpen}
        onClose={() => setCreateOpen(false)}
        onSuccess={(key) => handleMutateSuccess("Created", key)}
        onError={(msg) => addToast(msg, "error")}
      />

      {/* ---- Edit key ---- */}
      <EditModal
        item={editKey}
        onClose={() => setEditKey(null)}
        onSuccess={(key) => handleMutateSuccess("Updated", key)}
        onError={(msg) => addToast(msg, "error")}
      />

      {/* ---- Delete confirmation ---- */}
      <Modal
        isOpen={deleteTarget !== null}
        onClose={() => setDeleteTarget(null)}
        title="Delete Key"
      >
        <div className={styles.confirmBody}>
          <span className={styles.confirmIcon} aria-hidden="true">🗑</span>
          <p className={styles.confirmTitle}>
            Delete{" "}
            <span className={styles.confirmKey}>
              "{deleteTarget?.key}"
            </span>
            ?
          </p>
          <p className={styles.confirmSub}>This action cannot be undone.</p>
          <div className={styles.confirmActions}>
            <button
              className={styles.btnSecondary}
              type="button"
              onClick={() => setDeleteTarget(null)}
              disabled={deleting}
            >
              Cancel
            </button>
            <button
              className={styles.btnDanger}
              type="button"
              onClick={handleConfirmDelete}
              disabled={deleting}
              aria-label={`Confirm delete ${deleteTarget?.key}`}
            >
              {deleting ? "Deleting…" : "Delete"}
            </button>
          </div>
        </div>
      </Modal>

      {/* ---- Toasts ---- */}
      <Toast toasts={toasts} onDismiss={dismissToast} />
    </div>
  );
}

// =============================================================================
// KeyRow — a single table row
// =============================================================================

interface KeyRowProps {
  item: KeyInfo;
  onView: () => void;
  onEdit: () => void;
  onDelete: () => void;
}

function KeyRow({ item, onView, onEdit, onDelete }: KeyRowProps) {
  const ttlClass =
    item.ttl_seconds < 0
      ? styles.ttlPermanent
      : item.ttl_seconds < 10
      ? styles.ttlExpiring
      : styles.ttlNormal;

  return (
    <tr
      onClick={onView}
      title={`Click to view "${item.key}"`}
    >
      <td className={styles.colKey}>
        <span className={styles.cellKey}>{item.key}</span>
      </td>
      <td className={styles.colValue}>
        <span className={styles.cellValue}>
          {truncateValue(item.value, 60)}
        </span>
      </td>
      <td className={styles.colTtl}>
        <span className={`${styles.cellTtl} ${ttlClass}`}>
          {formatTtl(item.ttl_seconds)}
        </span>
      </td>
      <td className={styles.colActions}>
        <div
          className={styles.actions}
          onClick={(e) => e.stopPropagation()} // don't trigger row click
        >
          <button
            className={styles.actionBtnEdit}
            type="button"
            onClick={onEdit}
            aria-label={`Edit ${item.key}`}
          >
            Edit
          </button>
          <button
            className={styles.actionBtnDelete}
            type="button"
            onClick={onDelete}
            aria-label={`Delete ${item.key}`}
          >
            Delete
          </button>
        </div>
      </td>
    </tr>
  );
}

// =============================================================================
// ViewModal — read-only key detail
// =============================================================================

interface ViewModalProps {
  item: KeyInfo | null;
  onClose: () => void;
  onEdit: (item: KeyInfo) => void;
}

function ViewModal({ item, onClose, onEdit }: ViewModalProps) {
  if (!item) return null;

  const ttlDisplay =
    item.ttl_seconds < 0
      ? "Permanent (no expiry)"
      : item.ttl_seconds < 1
      ? "Expiring imminently"
      : `${formatTtl(item.ttl_seconds)} remaining`;

  return (
    <Modal isOpen={true} onClose={onClose} title="Key Detail">
      <div>
        <div className={styles.viewRow}>
          <span className={styles.viewLabel}>Key</span>
          <span className={styles.viewValue}>{item.key}</span>
        </div>
        <div className={styles.viewRow}>
          <span className={styles.viewLabel}>Value</span>
          <pre className={styles.viewValue}>{item.value}</pre>
        </div>
        <div className={styles.viewRow}>
          <span className={styles.viewLabel}>TTL</span>
          <span className={styles.viewValueInline}>{ttlDisplay}</span>
        </div>

        <div className={styles.modalFooter}>
          <button className={styles.btnSecondary} type="button" onClick={onClose}>
            Close
          </button>
          <button
            className={styles.btnPrimary}
            type="button"
            onClick={() => onEdit(item)}
          >
            Edit
          </button>
        </div>
      </div>
    </Modal>
  );
}

// =============================================================================
// CreateModal — create a new key
// =============================================================================

interface CreateModalProps {
  isOpen: boolean;
  onClose: () => void;
  onSuccess: (key: string) => void;
  onError: (msg: string) => void;
}

function CreateModal({ isOpen, onClose, onSuccess, onError }: CreateModalProps) {
  const [keyField, setKeyField]     = useState("");
  const [valueField, setValueField] = useState("");
  const [ttlMode, setTtlMode]       = useState<TtlMode>("permanent");
  const [ttlValue, setTtlValue]     = useState("");
  const [saving, setSaving]         = useState(false);
  const [formError, setFormError]   = useState<string | null>(null);

  // Reset form when modal opens/closes.
  useEffect(() => {
    if (isOpen) {
      setKeyField("");
      setValueField("");
      setTtlMode("permanent");
      setTtlValue("");
      setFormError(null);
    }
  }, [isOpen]);

  const handleSubmit = async (e: React.FormEvent) => {
    e.preventDefault();
    setFormError(null);

    const trimmedKey = keyField.trim();
    if (!trimmedKey) {
      setFormError("Key is required.");
      return;
    }
    if (valueField === "") {
      setFormError("Value cannot be empty.");
      return;
    }

    let ttl: number | undefined;
    if (ttlMode === "custom") {
      ttl = parseTtlFromForm(ttlMode, ttlValue);
      if (ttl === undefined) {
        setFormError("TTL must be a positive number of seconds.");
        return;
      }
    }

    setSaving(true);
    const result = await setKey(trimmedKey, valueField, ttl);
    setSaving(false);

    if (!result.ok) {
      setFormError(result.error);
      onError(`Failed to create "${trimmedKey}": ${result.error}`);
      return;
    }

    onSuccess(trimmedKey);
    onClose();
  };

  return (
    <Modal isOpen={isOpen} onClose={onClose} title="Create Key">
      <form onSubmit={handleSubmit} noValidate>
        <div className={styles.formGroup}>
          <label className={styles.label} htmlFor="create-key">
            Key <span className={styles.required} aria-hidden="true">*</span>
          </label>
          <input
            id="create-key"
            className={`${styles.input} ${styles.inputMono}`}
            type="text"
            value={keyField}
            onChange={(e) => setKeyField(e.target.value)}
            placeholder="e.g. user:1001"
            autoComplete="off"
            spellCheck={false}
            required
            disabled={saving}
          />
        </div>

        <div className={styles.formGroup}>
          <label className={styles.label} htmlFor="create-value">
            Value <span className={styles.required} aria-hidden="true">*</span>
          </label>
          <textarea
            id="create-value"
            className={`${styles.textarea} ${styles.textareaMono}`}
            value={valueField}
            onChange={(e) => setValueField(e.target.value)}
            placeholder="Enter value…"
            spellCheck={false}
            required
            disabled={saving}
          />
        </div>

        <TtlField
          mode={ttlMode}
          value={ttlValue}
          onModeChange={setTtlMode}
          onValueChange={setTtlValue}
          disabled={saving}
        />

        {formError && (
          <p className={styles.fieldError} role="alert">{formError}</p>
        )}

        <div className={styles.modalFooter}>
          <button
            type="button"
            className={styles.btnSecondary}
            onClick={onClose}
            disabled={saving}
          >
            Cancel
          </button>
          <button
            type="submit"
            className={styles.btnPrimary}
            disabled={saving}
          >
            {saving ? "Creating…" : "Create Key"}
          </button>
        </div>
      </form>
    </Modal>
  );
}

// =============================================================================
// EditModal — update an existing key
// =============================================================================

interface EditModalProps {
  item: KeyInfo | null;
  onClose: () => void;
  onSuccess: (key: string) => void;
  onError: (msg: string) => void;
}

function EditModal({ item, onClose, onSuccess, onError }: EditModalProps) {
  const [valueField, setValueField] = useState("");
  const [ttlMode, setTtlMode]       = useState<TtlMode>("permanent");
  const [ttlValue, setTtlValue]     = useState("");
  const [saving, setSaving]         = useState(false);
  const [formError, setFormError]   = useState<string | null>(null);

  // Populate form when item changes (modal opens with different key).
  useEffect(() => {
    if (item) {
      // Fetch the freshest value from the backend so we don't show stale data.
      let cancelled = false;
      (async () => {
        const res = await getKey(item.key);
        if (cancelled) return;
        if (res.ok) {
          setValueField(res.data.value);
        } else {
          setValueField(item.value);
        }
      })();

      // Pre-fill TTL from the listing data.
      if (item.ttl_seconds < 0) {
        setTtlMode("permanent");
        setTtlValue("");
      } else {
        setTtlMode("custom");
        setTtlValue(String(Math.ceil(item.ttl_seconds)));
      }
      setFormError(null);

      return () => { cancelled = true; };
    }
  }, [item]);

  if (!item) return null;

  const handleSubmit = async (e: React.FormEvent) => {
    e.preventDefault();
    setFormError(null);

    if (valueField === "") {
      setFormError("Value cannot be empty.");
      return;
    }

    let ttl: number | undefined;
    if (ttlMode === "custom") {
      ttl = parseTtlFromForm(ttlMode, ttlValue);
      if (ttl === undefined) {
        setFormError("TTL must be a positive number of seconds.");
        return;
      }
    }

    setSaving(true);
    const result = await setKey(item.key, valueField, ttl);
    setSaving(false);

    if (!result.ok) {
      setFormError(result.error);
      onError(`Failed to update "${item.key}": ${result.error}`);
      return;
    }

    onSuccess(item.key);
    onClose();
  };

  return (
    <Modal isOpen={true} onClose={onClose} title={`Edit "${item.key}"`}>
      <form onSubmit={handleSubmit} noValidate>
        <div className={styles.formGroup}>
          <label className={styles.label} htmlFor="edit-key">Key</label>
          <input
            id="edit-key"
            className={`${styles.input} ${styles.inputMono}`}
            type="text"
            value={item.key}
            readOnly
            aria-readonly="true"
          />
          <span className={styles.fieldHint}>
            Keys cannot be renamed. Delete and recreate to change the key name.
          </span>
        </div>

        <div className={styles.formGroup}>
          <label className={styles.label} htmlFor="edit-value">
            Value <span className={styles.required} aria-hidden="true">*</span>
          </label>
          <textarea
            id="edit-value"
            className={`${styles.textarea} ${styles.textareaMono}`}
            value={valueField}
            onChange={(e) => setValueField(e.target.value)}
            spellCheck={false}
            required
            disabled={saving}
          />
        </div>

        <TtlField
          mode={ttlMode}
          value={ttlValue}
          onModeChange={setTtlMode}
          onValueChange={setTtlValue}
          disabled={saving}
        />

        {formError && (
          <p className={styles.fieldError} role="alert">{formError}</p>
        )}

        <div className={styles.modalFooter}>
          <button
            type="button"
            className={styles.btnSecondary}
            onClick={onClose}
            disabled={saving}
          >
            Cancel
          </button>
          <button
            type="submit"
            className={styles.btnPrimary}
            disabled={saving}
          >
            {saving ? "Saving…" : "Save Changes"}
          </button>
        </div>
      </form>
    </Modal>
  );
}

// =============================================================================
// TtlField — shared TTL input for create/edit forms
// =============================================================================

interface TtlFieldProps {
  mode: TtlMode;
  value: string;
  onModeChange: (m: TtlMode) => void;
  onValueChange: (v: string) => void;
  disabled?: boolean;
}

function TtlField({
  mode,
  value,
  onModeChange,
  onValueChange,
  disabled,
}: TtlFieldProps) {
  return (
    <div className={styles.formGroup}>
      <label className={styles.label} htmlFor="ttl-mode">TTL</label>
      <div className={styles.ttlRow}>
        <select
          id="ttl-mode"
          className={styles.select}
          value={mode}
          onChange={(e) => onModeChange(e.target.value as TtlMode)}
          disabled={disabled}
          aria-label="TTL type"
        >
          <option value="permanent">Permanent</option>
          <option value="custom">Expires in…</option>
        </select>
        {mode === "custom" && (
          <input
            className={styles.input}
            type="number"
            min="1"
            step="1"
            value={value}
            onChange={(e) => onValueChange(e.target.value)}
            placeholder="Seconds, e.g. 3600"
            disabled={disabled}
            aria-label="TTL seconds"
          />
        )}
      </div>
      {mode === "custom" && (
        <span className={styles.fieldHint}>
          Seconds until the key expires. Must be &gt; 0.
        </span>
      )}
    </div>
  );
}
