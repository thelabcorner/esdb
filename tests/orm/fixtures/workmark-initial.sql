CREATE TABLE projects(
  id BLOB PRIMARY KEY CHECK(length(id) = 16), name TEXT NOT NULL,
  created_at INTEGER NOT NULL, archived INTEGER NOT NULL DEFAULT 0);

CREATE TABLE documents(
  id BLOB PRIMARY KEY CHECK(length(id) = 16), project_id BLOB REFERENCES projects(id),
  display_name TEXT NOT NULL, created_at INTEGER NOT NULL,
  last_seen_at INTEGER NOT NULL,
  lineage_parent_id BLOB REFERENCES documents(id),
  CHECK (lineage_parent_id IS NULL OR lineage_parent_id != id));

CREATE TABLE document_incarnations(
  id INTEGER PRIMARY KEY, document_id BLOB NOT NULL REFERENCES documents(id),
  locator_kind INTEGER NOT NULL,
  locator TEXT,
  file_identity TEXT, size INTEGER, mtime INTEGER,
  stamp_present INTEGER NOT NULL, first_seen_at INTEGER, last_seen_at INTEGER);

CREATE TABLE versions(
  id BLOB PRIMARY KEY CHECK(length(id) = 16),
  document_id BLOB NOT NULL REFERENCES documents(id),
  created_on_branch_id BLOB NOT NULL,
  root_manifest_object_id BLOB NOT NULL REFERENCES storage_objects(object_id),
  created_at INTEGER NOT NULL,
  reason INTEGER NOT NULL,
  level INTEGER NOT NULL,
  label TEXT, description TEXT,
  session_id BLOB REFERENCES sessions(id),
  change_summary BLOB,
  undo_tag TEXT,
  integrity_state INTEGER NOT NULL DEFAULT 0,
  UNIQUE(id, document_id),
  FOREIGN KEY(created_on_branch_id, document_id)
    REFERENCES branches(id, document_id));

CREATE TABLE version_parents(
  document_id BLOB NOT NULL REFERENCES documents(id),
  version_id BLOB NOT NULL,
  parent_id BLOB NOT NULL,
  ordinal INTEGER NOT NULL CHECK(ordinal >= 0),
  PRIMARY KEY(version_id, parent_id),
  UNIQUE(version_id, ordinal),
  CHECK(version_id != parent_id),
  FOREIGN KEY(version_id, document_id) REFERENCES versions(id, document_id),
  FOREIGN KEY(parent_id, document_id) REFERENCES versions(id, document_id));

CREATE TABLE branches(
  id BLOB PRIMARY KEY CHECK(length(id) = 16),
  document_id BLOB NOT NULL REFERENCES documents(id),
  name TEXT NOT NULL,
  head_version_id BLOB,
  created_from_version_id BLOB,
  created_at INTEGER NOT NULL,
  state INTEGER NOT NULL DEFAULT 0 CHECK(state BETWEEN 0 AND 2),
  deleted_at INTEGER,
  auto_named INTEGER NOT NULL DEFAULT 1,
  UNIQUE(id, document_id),
  CHECK ((state = 2 AND head_version_id IS NULL AND deleted_at IS NOT NULL) OR
         (state != 2 AND deleted_at IS NULL)),
  FOREIGN KEY(head_version_id, document_id) REFERENCES versions(id, document_id),
  FOREIGN KEY(created_from_version_id, document_id) REFERENCES versions(id, document_id));

CREATE TABLE refs(
  name TEXT NOT NULL,
  document_id BLOB NOT NULL REFERENCES documents(id),
  version_id BLOB NOT NULL,
  kind INTEGER NOT NULL,
  created_at INTEGER NOT NULL,
  PRIMARY KEY(document_id, name),
  FOREIGN KEY(version_id, document_id) REFERENCES versions(id, document_id));

CREATE TABLE storage_objects(
  object_id BLOB PRIMARY KEY,
  size INTEGER NOT NULL, stored_size INTEGER NOT NULL,
  compression INTEGER NOT NULL,
  created_at INTEGER NOT NULL,
  pack_id BLOB REFERENCES storage_packs(id),
  pack_offset INTEGER,
  verified_at INTEGER,
  CHECK ((pack_id IS NULL AND pack_offset IS NULL) OR
         (pack_id IS NOT NULL AND pack_offset IS NOT NULL)));

CREATE TABLE storage_packs(
  id BLOB PRIMARY KEY CHECK(length(id) = 16), relative_path TEXT NOT NULL, size INTEGER NOT NULL,
  object_count INTEGER NOT NULL, created_at INTEGER NOT NULL);

CREATE TABLE object_identities(
  workmark_object_id BLOB PRIMARY KEY CHECK(length(workmark_object_id) = 16),
  document_id BLOB NOT NULL REFERENCES documents(id),
  host_uuid BLOB CHECK(host_uuid IS NULL OR length(host_uuid) = 16),
  art_type INTEGER,
  first_seen_version BLOB,
  last_seen_version BLOB,
  lineage_parent BLOB,
  UNIQUE(workmark_object_id, document_id),
  FOREIGN KEY(first_seen_version, document_id) REFERENCES versions(id, document_id),
  FOREIGN KEY(last_seen_version, document_id) REFERENCES versions(id, document_id),
  FOREIGN KEY(lineage_parent, document_id)
    REFERENCES object_identities(workmark_object_id, document_id));

CREATE TABLE version_change_index(
  document_id BLOB NOT NULL REFERENCES documents(id),
  version_id BLOB NOT NULL,
  workmark_object_id BLOB NOT NULL,
  change_kind INTEGER NOT NULL,
  PRIMARY KEY(version_id, workmark_object_id),
  FOREIGN KEY(version_id, document_id) REFERENCES versions(id, document_id),
  FOREIGN KEY(workmark_object_id, document_id)
    REFERENCES object_identities(workmark_object_id, document_id));

CREATE TABLE previews(
  version_id BLOB NOT NULL REFERENCES versions(id),
  kind INTEGER NOT NULL,
  object_id BLOB NOT NULL REFERENCES storage_objects(object_id),
  width INTEGER, height INTEGER,
  PRIMARY KEY(version_id, kind));

CREATE TABLE gc_version_pins(
  version_id BLOB NOT NULL REFERENCES versions(id),
  owner_operation_id BLOB NOT NULL CHECK(length(owner_operation_id) = 16),
  reason INTEGER NOT NULL,
  created_at INTEGER NOT NULL,
  PRIMARY KEY(version_id, owner_operation_id));

CREATE TABLE integrity_events(
  id INTEGER PRIMARY KEY, detected_at INTEGER NOT NULL, kind INTEGER NOT NULL,
  subject BLOB, detail TEXT, resolved_at INTEGER);

CREATE TABLE sessions(
  id BLOB PRIMARY KEY CHECK(length(id) = 16),
  started_at_utc INTEGER NOT NULL, ended_at_utc INTEGER,
  host_version TEXT, workmark_version TEXT, close_reason INTEGER,
  CHECK(ended_at_utc IS NULL OR ended_at_utc >= started_at_utc));

CREATE TABLE activity_intervals(
  id INTEGER PRIMARY KEY,
  document_id BLOB REFERENCES documents(id),
  direct_project_id BLOB REFERENCES projects(id),
  session_id BLOB NOT NULL REFERENCES sessions(id),
  started_at_utc INTEGER NOT NULL, ended_at_utc INTEGER,
  utc_offset_min INTEGER NOT NULL CHECK(utc_offset_min BETWEEN -840 AND 840),
  active_ms INTEGER NOT NULL DEFAULT 0, wall_span_ms INTEGER NOT NULL DEFAULT 0,
  edit_event_count INTEGER NOT NULL DEFAULT 0,
  interaction_event_count INTEGER NOT NULL DEFAULT 0,
  credited_gap_ms INTEGER NOT NULL DEFAULT 0,
  close_reason INTEGER, confidence INTEGER NOT NULL DEFAULT 0, source INTEGER NOT NULL DEFAULT 0,
  CHECK ((document_id IS NOT NULL AND direct_project_id IS NULL) OR
         (document_id IS NULL AND direct_project_id IS NOT NULL)),
  CHECK(ended_at_utc IS NULL OR ended_at_utc >= started_at_utc),
  CHECK(active_ms >= 0 AND wall_span_ms >= 0 AND active_ms <= wall_span_ms),
  CHECK(edit_event_count >= 0 AND interaction_event_count >= 0),
  CHECK(credited_gap_ms >= 0 AND credited_gap_ms <= active_ms));

CREATE TABLE idle_events(
  id INTEGER PRIMARY KEY, session_id BLOB NOT NULL REFERENCES sessions(id),
  started_at_utc INTEGER NOT NULL, ended_at_utc INTEGER, kind INTEGER,
  credited INTEGER NOT NULL CHECK(credited IN (0,1)),
  CHECK(ended_at_utc IS NULL OR ended_at_utc >= started_at_utc));

CREATE TABLE manual_adjustments(
  id INTEGER PRIMARY KEY, interval_id INTEGER REFERENCES activity_intervals(id),
  kind INTEGER NOT NULL, before_json TEXT, after_json TEXT,
  reason TEXT, created_at_utc INTEGER NOT NULL);

CREATE TABLE time_version_links(
  version_id BLOB NOT NULL,
  document_id BLOB NOT NULL REFERENCES documents(id),
  active_ms INTEGER NOT NULL CHECK(active_ms >= 0), method INTEGER NOT NULL,
  PRIMARY KEY(version_id),
  FOREIGN KEY(version_id, document_id) REFERENCES versions(id, document_id));

CREATE TABLE daily_document_rollups(
  local_day INTEGER NOT NULL,
  document_id BLOB NOT NULL REFERENCES documents(id),
  active_ms INTEGER NOT NULL,
  edit_event_count INTEGER NOT NULL,
  interaction_event_count INTEGER NOT NULL,
  credited_gap_ms INTEGER NOT NULL,
  computed_at INTEGER NOT NULL,
  PRIMARY KEY(local_day, document_id),
  CHECK(active_ms >= 0 AND edit_event_count >= 0 AND interaction_event_count >= 0),
  CHECK(credited_gap_ms >= 0 AND credited_gap_ms <= active_ms));

CREATE TABLE daily_direct_project_rollups(
  local_day INTEGER NOT NULL,
  project_id BLOB NOT NULL REFERENCES projects(id),
  active_ms INTEGER NOT NULL,
  edit_event_count INTEGER NOT NULL,
  interaction_event_count INTEGER NOT NULL,
  credited_gap_ms INTEGER NOT NULL,
  computed_at INTEGER NOT NULL,
  PRIMARY KEY(local_day, project_id),
  CHECK(active_ms >= 0 AND edit_event_count >= 0 AND interaction_event_count >= 0),
  CHECK(credited_gap_ms >= 0 AND credited_gap_ms <= active_ms));

CREATE TABLE entitlements(
  id INTEGER PRIMARY KEY CHECK(id = 1), tier INTEGER NOT NULL,
  feature_bits INTEGER NOT NULL, source INTEGER NOT NULL,
  lease_sequence INTEGER, hard_expiry_ms INTEGER, updated_at INTEGER NOT NULL);

CREATE TABLE settings(key TEXT PRIMARY KEY, value TEXT NOT NULL, schema_version INTEGER NOT NULL);

CREATE TABLE applied_operations(
  operation_id BLOB PRIMARY KEY CHECK(length(operation_id) = 16),
  op_code INTEGER NOT NULL,
  result_code INTEGER NOT NULL,
  result_payload BLOB NOT NULL CHECK(length(result_payload) <= 4096),
  applied_at INTEGER NOT NULL);

CREATE TABLE stream_cursors(
  session_id BLOB NOT NULL REFERENCES sessions(id),
  stream_kind INTEGER NOT NULL,
  last_source_seq INTEGER NOT NULL,
  PRIMARY KEY(session_id, stream_kind));

CREATE INDEX idx_documents_project
  ON documents(project_id);
CREATE INDEX idx_documents_lineage_parent
  ON documents(lineage_parent_id) WHERE lineage_parent_id IS NOT NULL;
CREATE INDEX idx_document_incarnations_document_seen
  ON document_incarnations(document_id, last_seen_at DESC);
CREATE INDEX idx_document_incarnations_locator
  ON document_incarnations(locator_kind, locator);
CREATE INDEX idx_document_incarnations_file_identity
  ON document_incarnations(file_identity) WHERE file_identity IS NOT NULL;
CREATE INDEX idx_versions_document_created
  ON versions(document_id, created_at DESC);
CREATE INDEX idx_versions_branch_created
  ON versions(created_on_branch_id, created_at DESC);
CREATE INDEX idx_versions_root_manifest
  ON versions(root_manifest_object_id);
CREATE INDEX idx_versions_session
  ON versions(session_id) WHERE session_id IS NOT NULL;
CREATE INDEX idx_version_parents_parent
  ON version_parents(parent_id, version_id);
CREATE INDEX idx_branches_document_state
  ON branches(document_id, state);
CREATE INDEX idx_branches_head
  ON branches(head_version_id) WHERE head_version_id IS NOT NULL;
CREATE INDEX idx_branches_created_from
  ON branches(created_from_version_id) WHERE created_from_version_id IS NOT NULL;
CREATE INDEX idx_refs_document_kind_created
  ON refs(document_id, kind, created_at DESC);
CREATE INDEX idx_refs_version
  ON refs(version_id);
CREATE INDEX idx_storage_objects_pack
  ON storage_objects(pack_id, pack_offset) WHERE pack_id IS NOT NULL;
CREATE INDEX idx_object_identities_document_uuid
  ON object_identities(document_id, host_uuid);
CREATE INDEX idx_object_identities_first_version
  ON object_identities(first_seen_version) WHERE first_seen_version IS NOT NULL;
CREATE INDEX idx_object_identities_last_version
  ON object_identities(last_seen_version) WHERE last_seen_version IS NOT NULL;
CREATE INDEX idx_object_identities_lineage_parent
  ON object_identities(lineage_parent) WHERE lineage_parent IS NOT NULL;
CREATE INDEX idx_version_change_object
  ON version_change_index(workmark_object_id, version_id);
CREATE INDEX idx_previews_object
  ON previews(object_id);
CREATE INDEX idx_integrity_events_unresolved
  ON integrity_events(detected_at) WHERE resolved_at IS NULL;
CREATE INDEX idx_sessions_open
  ON sessions(ended_at_utc) WHERE ended_at_utc IS NULL;
CREATE INDEX idx_activity_document_start
  ON activity_intervals(document_id, started_at_utc);
CREATE INDEX idx_activity_direct_project_start
  ON activity_intervals(direct_project_id, started_at_utc)
  WHERE direct_project_id IS NOT NULL;
CREATE INDEX idx_activity_session_start
  ON activity_intervals(session_id, started_at_utc);
CREATE INDEX idx_idle_session_start
  ON idle_events(session_id, started_at_utc);
CREATE INDEX idx_adjustments_interval_created
  ON manual_adjustments(interval_id, created_at_utc);
CREATE INDEX idx_time_version_document
  ON time_version_links(document_id, version_id);
CREATE INDEX idx_daily_document_document_day
  ON daily_document_rollups(document_id, local_day);
CREATE INDEX idx_daily_project_project_day
  ON daily_direct_project_rollups(project_id, local_day);
CREATE INDEX idx_gc_version_pins_owner
  ON gc_version_pins(owner_operation_id);
CREATE INDEX idx_applied_operations_applied_at
  ON applied_operations(applied_at);
