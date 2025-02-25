/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

"use strict";

const EventEmitter = require("devtools/shared/event-emitter");

loader.lazyRequireGetter(this, "StorageUI", "devtools/client/storage/ui", true);

class StoragePanel {
  constructor(panelWin, toolbox) {
    EventEmitter.decorate(this);

    this._toolbox = toolbox;
    this._target = toolbox.target;
    this._panelWin = panelWin;

    this.destroy = this.destroy.bind(this);
  }

  get target() {
    return this._toolbox.target;
  }

  get panelWindow() {
    return this._panelWin;
  }

  /**
   * open is effectively an asynchronous constructor
   */
  async open() {
    this.target.on("close", this.destroy);
    this._front = await this.target.getFront("storage");

    this.UI = new StorageUI(
      this._front,
      this._target,
      this._panelWin,
      this._toolbox
    );
    this.isReady = true;
    this.emit("ready");

    return this;
  }

  /**
   * Destroy the storage inspector.
   */
  destroy() {
    if (this._destroyed) {
      return;
    }
    this._destroyed = true;

    this.UI.destroy();
    this.UI = null;

    // Destroy front to ensure packet handler is removed from client
    this._front.destroy();
    this._front = null;

    this._target.off("close", this.destroy);
    this._target = null;
    this._toolbox = null;
    this._panelWin = null;
  }
}

exports.StoragePanel = StoragePanel;
