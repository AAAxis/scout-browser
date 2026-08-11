// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

import '../icons.html.js';
import '//resources/cr_elements/cr_icon/cr_icon.js';

import {CrLitElement} from '//resources/lit/v3_0/lit.rollup.js';

import {BookmarksService} from '../bookmarks_api.mojom-webui.js';
import type {BookmarkNode} from '../bookmarks_api.mojom-webui.js';

import {getCss} from './bookmark_tree_node.css.js';
import {getHtml} from './bookmark_tree_node.html.js';

// TODO(crbug.com/501483829): split this up into two different types.
export class BookmarkTreeNodeElement extends CrLitElement {
  static get is() {
    return 'webui-browser-bookmark-tree-node';
  }

  static override get styles() {
    return getCss();
  }

  override render() {
    return getHtml.bind(this)();
  }

  static override get properties() {
    return {
      addDialogType_: {type: String},
      addTitle_: {type: String},
      addUrl_: {type: String},
      node: {type: Object},
    };
  }

  accessor node: BookmarkNode = {
    folder: {
      id: {value: ''},
      title: '',
      children: [],
    },
  };

  private bookmarksService_ = BookmarksService.getRemote();
  protected accessor addDialogType_: 'url'|'folder'|null = null;
  protected accessor addTitle_: string = '';
  protected accessor addUrl_: string = '';

  protected onAddUrlClick(e: Event) {
    e.stopPropagation();
    e.preventDefault();
    this.showAddDialog_('url');
  }

  protected onAddFolderClick(e: Event) {
    e.stopPropagation();
    e.preventDefault();
    this.showAddDialog_('folder');
  }

  protected onAddTitleInput_(e: Event) {
    this.addTitle_ = (e.target as HTMLInputElement).value;
  }

  protected onAddUrlInput_(e: Event) {
    this.addUrl_ = (e.target as HTMLInputElement).value;
  }

  protected onAddDialogCancel_(e: Event) {
    e.stopPropagation();
    e.preventDefault();
    this.shadowRoot?.querySelector<HTMLDialogElement>('#addDialog')?.close();
    this.resetAddDialog_();
  }

  protected onAddDialogCancelClick_(e: Event) {
    this.onAddDialogCancel_(e);
  }

  protected onAddDialogSubmit_(e: Event) {
    e.stopPropagation();
    e.preventDefault();

    if (!this.node.folder) {
      return;
    }

    const parentId = this.node.folder.id!;
    if (this.addDialogType_ === 'url') {
      const title = this.addTitle_.trim() || 'New bookmark';
      const url = this.addUrl_.trim() || 'chrome://new-tab-page';
      const newUrlNode = {
        url: {
          id: null,
          title: title,
          url: url,
          faviconUrl: null,
        },
      };

      this.bookmarksService_.createBookmarkNode(parentId, null, newUrlNode);
    } else if (this.addDialogType_ === 'folder') {
      const title = this.addTitle_.trim() || 'New folder';
      const newFolderNode = {
        folder: {
          id: null,
          title: title,
          children: [],
        },
      };

      this.bookmarksService_.createBookmarkNode(parentId, null, newFolderNode);
    }

    this.shadowRoot?.querySelector<HTMLDialogElement>('#addDialog')?.close();
    this.resetAddDialog_();
  }

  protected onEditClick(e: Event) {
    e.stopPropagation();
    e.preventDefault();

    if (this.node.url) {
      const updatedNode = {
        url: {
          id: this.node.url.id!,
          title: 'has been updated',
          url: 'http://updated.somewhere',
          faviconUrl: null,
        },
      };

      this.bookmarksService_.updateBookmarkNode(updatedNode);
    } else if (this.node.folder) {
      const updatedNode = {
        folder: {
          id: this.node.folder.id!,
          title: 'updated folder',
          children: [],
        },
      };

      this.bookmarksService_.updateBookmarkNode(updatedNode);
    }
  }

  protected onMoveClick(e: Event) {
    e.stopPropagation();
    e.preventDefault();

    const host = (this.getRootNode() as ShadowRoot).host;

    if (host &&
        host.tagName.toLowerCase() === 'webui-browser-bookmark-tree-node') {
      const id = this.node.url ? this.node.url.id! : this.node.folder!.id!;

      const targetParentId = (host as BookmarkTreeNodeElement).node.folder!.id!;
      // always move to first element for now.
      this.bookmarksService_.moveBookmarkNode(id, targetParentId, 0);
    }
  }

  protected onDeleteClick(e: Event) {
    e.stopPropagation();
    e.preventDefault();

    const id = this.node.url ? this.node.url.id! : this.node.folder!.id!;

    this.bookmarksService_.deleteBookmarkNode(id);
  }

  private showAddDialog_(type: 'url'|'folder') {
    if (!this.node.folder) {
      return;
    }

    this.addDialogType_ = type;
    this.addTitle_ = '';
    this.addUrl_ = type === 'url' ? 'chrome://new-tab-page' : '';

    this.updateComplete.then(() => {
      const dialog =
          this.shadowRoot?.querySelector<HTMLDialogElement>('#addDialog');
      if (dialog && !dialog.open) {
        dialog.showModal();
      }
    });
  }

  private resetAddDialog_() {
    this.addDialogType_ = null;
    this.addTitle_ = '';
    this.addUrl_ = '';
  }
}

declare global {
  interface HTMLElementTagNameMap {
    'webui-browser-bookmark-tree-node': BookmarkTreeNodeElement;
  }
}

customElements.define(BookmarkTreeNodeElement.is, BookmarkTreeNodeElement);
