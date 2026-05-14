const fileList = document.getElementById("fileList");
const emptyState = document.getElementById("emptyState");
const selectAll = document.getElementById("selectAll");
const selectionCount = document.getElementById("selectionCount");
const deleteBtn = document.getElementById("deleteBtn");
const refreshBtn = document.getElementById("refreshBtn");

let files = [];
let selected = new Set();

function formatSize(bytes) {
  if (bytes < 1024) {
    return `${bytes} B`;
  }
  if (bytes < 1024 * 1024) {
    return `${(bytes / 1024).toFixed(1)} KB`;
  }
  return `${(bytes / (1024 * 1024)).toFixed(2)} MB`;
}

function updateSelectionState() {
  const total = files.length;
  const count = selected.size;
  selectionCount.textContent = `${count} selected`;
  deleteBtn.disabled = count === 0;
  if (total === 0) {
    selectAll.checked = false;
    selectAll.indeterminate = false;
  } else {
    selectAll.checked = count === total;
    selectAll.indeterminate = count > 0 && count < total;
  }
}

function renderRow(file, index) {
  const row = document.createElement("div");
  row.className = "row";
  row.style.animationDelay = `${index * 40}ms`;

  const checkCell = document.createElement("div");
  const checkbox = document.createElement("input");
  checkbox.type = "checkbox";
  checkbox.dataset.name = file.name;
  checkbox.checked = selected.has(file.name);
  checkbox.addEventListener("change", () => {
    if (checkbox.checked) {
      selected.add(file.name);
    } else {
      selected.delete(file.name);
    }
    updateSelectionState();
  });
  checkCell.appendChild(checkbox);

  const nameCell = document.createElement("div");
  const link = document.createElement("a");
  link.className = "file-link";
  link.href = `/download?name=${encodeURIComponent(file.name)}`;
  link.textContent = file.name;
  nameCell.appendChild(link);

  const sizeCell = document.createElement("div");
  sizeCell.className = "right";
  sizeCell.textContent = formatSize(file.size || 0);

  const actionCell = document.createElement("div");
  actionCell.className = "right";
  const actionLink = document.createElement("a");
  actionLink.className = "action-link";
  actionLink.href = `/download?name=${encodeURIComponent(file.name)}`;
  actionLink.textContent = "Download";
  actionCell.appendChild(actionLink);

  row.appendChild(checkCell);
  row.appendChild(nameCell);
  row.appendChild(sizeCell);
  row.appendChild(actionCell);

  return row;
}

function renderList() {
  fileList.innerHTML = "";
  if (files.length === 0) {
    emptyState.classList.remove("hidden");
  } else {
    emptyState.classList.add("hidden");
  }

  files.forEach((file, index) => {
    fileList.appendChild(renderRow(file, index));
  });

  updateSelectionState();
}

async function fetchFiles() {
  try {
    const response = await fetch("/api/files", { cache: "no-store" });
    if (!response.ok) {
      throw new Error("Failed to load files");
    }
    const data = await response.json();
    files = Array.isArray(data.files) ? data.files : [];
    const available = new Set(files.map((file) => file.name));
    selected = new Set([...selected].filter((name) => available.has(name)));
    renderList();
  } catch (err) {
    console.error(err);
    files = [];
    selected.clear();
    renderList();
  }
}

async function deleteSelected() {
  if (selected.size === 0) {
    return;
  }
  if (!confirm(`Delete ${selected.size} file(s)? This cannot be undone.`)) {
    return;
  }

  deleteBtn.disabled = true;
  try {
    const payload = { files: [...selected] };
    const response = await fetch("/api/delete", {
      method: "POST",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify(payload),
    });
    if (!response.ok) {
      throw new Error("Delete failed");
    }
    selected.clear();
    await fetchFiles();
  } catch (err) {
    console.error(err);
    await fetchFiles();
  }
}

selectAll.addEventListener("change", () => {
  if (selectAll.checked) {
    selected = new Set(files.map((file) => file.name));
  } else {
    selected.clear();
  }
  renderList();
});

deleteBtn.addEventListener("click", deleteSelected);
refreshBtn.addEventListener("click", fetchFiles);

fetchFiles();
