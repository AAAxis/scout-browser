// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

import {html} from 'chrome://resources/lit/v3_0/lit.rollup.js';

import type {BookmarkTreeNodeElement} from './bookmark_tree_node.js';

export function getHtml(this: BookmarkTreeNodeElement) {
  // clang-format off
  return html`
${this.node.folder ? html`
  <details>
    <summary>
      ${this.node.folder.title || 'Bookmarks'}
      <button @click="${this.onAddUrlClick}">+URL</button>
      <button @click="${this.onAddFolderClick}">+Folder</button>
      ${this.node.folder.id?.value ? html`
        <button @click="${this.onEditClick}">Edit</button>
        <button @click="${this.onMoveClick}">Move</button>
        <button @click="${this.onDeleteClick}">Delete</button>
      ` : ''}
    </summary>
    <div class="folder-children">
      ${this.node.folder.children.map(item => html`
        <webui-browser-bookmark-tree-node
            id="${item.folder?.id?.value || item.url?.id?.value}"
            .node="${item}">
        </webui-browser-bookmark-tree-node>
      `)}
    </div>
  </details>
  ${this.addDialogType_ ? html`
    <dialog id="addDialog" @cancel="${this.onAddDialogCancel_}">
      <form @submit="${this.onAddDialogSubmit_}">
        <h2>
          ${this.addDialogType_ === 'url' ? 'Add bookmark' : 'Add folder'}
        </h2>
        <label>
          Name
          <input type="text" .value="${this.addTitle_}"
              @input="${this.onAddTitleInput_}" autofocus>
        </label>
        ${this.addDialogType_ === 'url' ? html`
          <label>
            URL
            <input type="text" .value="${this.addUrl_}"
                @input="${this.onAddUrlInput_}">
          </label>
        ` : ''}
        <div class="dialog-actions">
          <button type="button" @click="${this.onAddDialogCancelClick_}">
            Cancel
          </button>
          <button type="submit">
            Add
          </button>
        </div>
      </form>
    </dialog>
  ` : ''}
` : ''}
${this.node.url ? html`
  <div class="bookmark-item">
    ${this.node.url.faviconUrl ? html`
      <img class="bookmark-icon" src="${this.node.url.faviconUrl}" width="16"
           height="16">
    ` : html`
      <cr-icon class="bookmark-icon" icon="webui-browser:bookmark"></cr-icon>
    `}
    <a href="${this.node.url.url}" target="_blank">
      ${this.node.url.title || this.node.url.url}
    </a>
    <button @click="${this.onEditClick}">Edit</button>
    <button @click="${this.onMoveClick}">Move</button>
    <button @click="${this.onDeleteClick}">Delete</button>
  </div>
` : ''}
`;
  // clang-format on
}
