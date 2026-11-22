// API Calls
//Module.api.terminal.write(str)
//Module.api.terminal.getSize()
//Module.api.export_file(_filename, file, url)
//Module.api.wgetRequests.getNextHandle();
//Module.api.wgetRequests.instance[handle];
//Module.api.update_downloading
//Module.api.display
//Module.api.net

mergeInto(LibraryManager.library, {

	console_write: function(opaque, buf, len) {
		//console.log('console_write', opaque, buf, len);
		// Pass the raw bytes (a HEAP view) straight through; the engine copies
		// them out. Avoids a bytes->string->bytes round-trip and the argument
		// stack-overflow risk of String.fromCharCode.apply on large writes.
		Module.api.terminal.write(HEAPU8.subarray(buf, buf + len));
	},

	console_get_size: function(pw, ph) {
		let s = Module.api.terminal.getSize();
		//console.log('console_get_size', pw, ph, s[0], s[1]);
		HEAPU32[pw >> 2] = s[0];
		HEAPU32[ph >> 2] = s[1];
	},

	fs_export_file: function(filename, buf, len) {
		//console.log('fs_export_file', filename, buf, len);
		let _filename = UTF8ToString(filename);
		// console.log("exporting " + _filename);
		let data = HEAPU8.subarray(buf, buf + len);
		let file = new Blob([data], {type: "application/octet-stream"});
		let url = URL.createObjectURL(file);

		Module.api.export_file(_filename, file, url);
	},

	emscripten_async_wget3_data: function(url, request, user, password, post_data, post_data_len, arg, free, onload, onerror, onprogress) {
		//console.log('emscripten_async_wget3_data', url, request, user, password, post_data, post_data_len, arg, free, onload, onerror, onprogress);
		let _url = UTF8ToString(url);
		let _request = UTF8ToString(request);
		let _user = user ? UTF8ToString(user) : null;
		let _password = password ? UTF8ToString(password) : null;
		let _post_data = _request == "POST" ? HEAPU8.subarray(post_data, post_data + post_data_len) : null;

		let http = new XMLHttpRequest();
		http.open(_request, _url, true);
		http.responseType = 'arraybuffer';
		if (_user) {
			http.setRequestHeader("Authorization", "Basic " + btoa(_user + ':' + _password));
		}
		
		let handle = Module.api.wgetRequests.new(http);

		// Load
		http.onload = function http_onload(e) {
			//console.log('emscripten_async_wget3_data > onload', _request, _url, e);
			if (http.status == 200 || _url.substr(0,4).toLowerCase() != "http") {
				var byteArray = new Uint8Array(http.response);
				var buffer = _malloc(byteArray.length);
				HEAPU8.set(byteArray, buffer);
				//console.log('emscripten_async_wget3_data > onload', _request, _url, http.response, handle, arg, buffer, byteArray.length);
				if (onload) {
					wasmTable.get(onload).apply(null, [handle, arg, buffer, byteArray.length]);
				}
				if (free) _free(buffer);
			} else {
				if (onerror) {
					wasmTable.get(onerror).apply(null, [handle, arg, http.status, http.statusText]);
				}
			}
			Module.api.wgetRequests.remove(handle);
		};

		// Error
		http.onerror = function http_onerror(e) {
			//console.log('emscripten_async_wget3_data > onerror', _request, _url, e);
			if (onerror) {
				wasmTable.get(onerror).apply(null, [handle, arg, http.status, http.statusText]);
			}
			Module.api.wgetRequests.remove(handle);
		};

		// Progress
		http.onprogress = function http_onprogress(e) {
			//console.log('emscripten_async_wget3_data > onprogress', _request, _url, e);
			if (onprogress) {
				wasmTable.get(onprogress).apply(null, [handle, arg, e.loaded, e.lengthComputable || e.lengthComputable === undefined ? e.total : 0]);
			}
		};

		// Abort
		http.onabort = function http_onabort(e) {
			//console.log('onabort > onprogress', e);
			Module.api.wgetRequests.remove(handle);
		};

		// Useful because the browser can limit the number of redirection
		try {
			if (http.channel instanceof Ci.nsIHttpChannel)
			http.channel.redirectionLimit = 0;
		} catch (ex) { /* whatever */ }

		if (_request == "POST") {
			// Send the proper header information along with the request
			http.setRequestHeader("Content-type", "application/octet-stream");
			http.setRequestHeader("Content-length", post_data_len);
			http.setRequestHeader("Connection", "close");
			http.send(_post_data);
		} else {
			http.send(null);
		}

		return handle;
	},

	fs_wget_update_downloading: function (flag) {
		//console.log('fs_wget_update_downloading', flag);
		Module.api.downloading.update(Boolean(flag));
	},
	
	fb_refresh: function(opaque, data, x, y, w, h, stride) {
		//console.log('fb_refresh', opaque, data, x, y, w, h, stride);
		let display = Module.api.display;
		/* current x = 0 and w = width for all refreshes */
		// console.log("fb_refresh: x=" + x + " y=" + y + " w=" + w + " h=" + h);
		/* Write one packed RGBA word per pixel via a Uint32Array view over the
		 * ImageData, instead of four byte stores. Browsers are little-endian, so
		 * the packed layout is 0xAABBGGRR = A<<24 | B<<16 | G<<8 | R. The source
		 * word from the guest framebuffer is 0x00RRGGBB. Cache the view on the
		 * display object; rebuild it if the ImageData buffer changes (resize). */
		let out32 = display._out32;
		if (!out32 || out32.buffer !== display.image.data.buffer) {
			out32 = display._out32 = new Uint32Array(display.image.data.buffer);
		}
		let dst_pos1 = (y * display.width + x);
		for (let i = 0; i < h; i = (i + 1) | 0) {
			let src = data >> 2;
			let dst_pos = dst_pos1;
			for (let j = 0; j < w; j = (j + 1) | 0) {
				let v = HEAPU32[src];
				/* 0x00RRGGBB -> 0xFFBBGGRR */
				out32[dst_pos] = 0xff000000 | ((v & 0xff) << 16) | (v & 0xff00) | ((v >> 16) & 0xff);
				src = (src + 1) | 0;
				dst_pos = (dst_pos + 1) | 0;
			}
			data = (data + stride) | 0;
			dst_pos1 = (dst_pos1 + display.width) | 0;
		}

		display.ctx.putImageData(display.image, 0, 0, x, y, w, h);
	},

	net_recv_packet: function(bs, buf, len) {
		//console.log('net_recv_packet', bs, buf, len);
		if (Module.api.net) {
			Module.api.net.recvPacket(HEAPU8.subarray(buf, buf + len));
		}
	},

	/* JS-backed 9p volumes (see fs_js.c + jsLab's lib/volumes/Fs9pStore.js).
	 * All synchronous: they operate on the in-memory store registered at
	 * Module.api.fs9p (list = handle->store, byName = name->store). Return
	 * values are 0 / byte-count on success and negated P9 errno on failure. */
	js_fs_mount: function(namePtr) {
		if (!Module.api.fs9p) return -1;
		let store = Module.api.fs9p.byName[UTF8ToString(namePtr)];
		return store ? store.vol : -1;
	},

	js_fs_stat: function(vol, pathPtr, pmode, pino, psize, pmtime) {
		let store = Module.api.fs9p && Module.api.fs9p.list[vol];
		if (!store) return -2; /* -ENOENT */
		let r = store.stat(UTF8ToString(pathPtr));
		if (!r) return -2;
		HEAPU32[pmode >> 2] = r.mode;
		HEAPU32[pino >> 2] = r.ino;
		HEAPF64[psize >> 3] = r.size;
		HEAPF64[pmtime >> 3] = r.mtime;
		return 0;
	},

	js_fs_readdir: function(vol, pathPtr, index, namePtr, nameSize, pmode, pino) {
		let store = Module.api.fs9p && Module.api.fs9p.list[vol];
		if (!store) return -1;
		let e = store.readdir(UTF8ToString(pathPtr), index);
		if (!e) return -1;
		let enc = Module.api._textEncoder || (Module.api._textEncoder = new TextEncoder());
		let nameBytes = enc.encode(e.name);
		if (nameBytes.length + 1 > nameSize) return -1;
		HEAPU8.set(nameBytes, namePtr);
		HEAPU8[namePtr + nameBytes.length] = 0;
		HEAPU32[pmode >> 2] = e.mode;
		HEAPU32[pino >> 2] = e.ino;
		return nameBytes.length;
	},

	js_fs_read: function(vol, pathPtr, offset, buf, count) {
		let store = Module.api.fs9p && Module.api.fs9p.list[vol];
		if (!store) return -5; /* -EIO */
		let data = store.read(UTF8ToString(pathPtr), offset, count);
		if (data === null) return -2;
		HEAPU8.set(data, buf);
		return data.length;
	},

	js_fs_write: function(vol, pathPtr, offset, buf, count) {
		let store = Module.api.fs9p && Module.api.fs9p.list[vol];
		if (!store) return -5;
		return store.write(UTF8ToString(pathPtr), offset, HEAPU8.subarray(buf, buf + count));
	},

	js_fs_create: function(vol, pathPtr, mode, isDir) {
		let store = Module.api.fs9p && Module.api.fs9p.list[vol];
		if (!store) return -5;
		return store.create(UTF8ToString(pathPtr), mode, !!isDir);
	},

	js_fs_truncate: function(vol, pathPtr, size) {
		let store = Module.api.fs9p && Module.api.fs9p.list[vol];
		if (!store) return -5;
		return store.truncate(UTF8ToString(pathPtr), size);
	},

	js_fs_chmod: function(vol, pathPtr, mode) {
		let store = Module.api.fs9p && Module.api.fs9p.list[vol];
		if (!store) return -5;
		return store.chmod(UTF8ToString(pathPtr), mode);
	},

	js_fs_utime: function(vol, pathPtr, mtime) {
		let store = Module.api.fs9p && Module.api.fs9p.list[vol];
		if (!store) return -5;
		return store.utime(UTF8ToString(pathPtr), mtime);
	},

	js_fs_unlink: function(vol, pathPtr) {
		let store = Module.api.fs9p && Module.api.fs9p.list[vol];
		if (!store) return -5;
		return store.unlink(UTF8ToString(pathPtr));
	},

	js_fs_rename: function(vol, fromPtr, toPtr) {
		let store = Module.api.fs9p && Module.api.fs9p.list[vol];
		if (!store) return -5;
		return store.rename(UTF8ToString(fromPtr), UTF8ToString(toPtr));
	},

	/* JS-backed virtio-block disks (see block_js.c + the volume backends).
	 * read/write are async (backend promise) and completed by calling the
	 * exported _js_block_complete(dev, ret); virtio keeps only one request in
	 * flight. Each entry in Module.api.blk.list is { backend, sectorCount }. */
	js_block_mount: function(namePtr) {
		if (!Module.api.blk) return -1;
		let e = Module.api.blk.byName[UTF8ToString(namePtr)];
		return e ? e.vol : -1;
	},

	js_block_sectors: function(vol) {
		let e = Module.api.blk && Module.api.blk.list[vol];
		return e ? e.sectorCount : 0;
	},

	// Module.api.blk.pending counts requests in flight: a snapshot waits for 0.
	js_block_read: function(dev, vol, sector, buf, nsectors) {
		let e = Module.api.blk && Module.api.blk.list[vol];
		if (!e) { _js_block_complete(dev, -5); return; }
		let blk = Module.api.blk;
		blk.pending = (blk.pending || 0) + 1;
		e.backend.readSectors(sector, nsectors).then(function(data) {
			blk.pending--;
			// re-read HEAPU8: memory may have grown while awaiting
			HEAPU8.set(data.subarray(0, nsectors * 512), buf);
			_js_block_complete(dev, 0);
		}).catch(function() { blk.pending--; _js_block_complete(dev, -5); });
	},

	js_block_write: function(dev, vol, sector, buf, nsectors) {
		let e = Module.api.blk && Module.api.blk.list[vol];
		if (!e) { _js_block_complete(dev, -5); return; }
		let blk = Module.api.blk;
		blk.pending = (blk.pending || 0) + 1;
		// COPY now: virtio frees buf right after write_async returns.
		let data = HEAPU8.slice(buf, buf + nsectors * 512);
		e.backend.writeSectors(sector, data).then(function() {
			blk.pending--;
			_js_block_complete(dev, 0);
		}).catch(function() { blk.pending--; _js_block_complete(dev, -5); });
	},

	/* file buffer API */
	file_buffer_get_new_handle: function() {
		//console.log('file_buffer_get_new_handle');
		if (typeof Module.api.fbuf_table == "undefined") {
			Module.api.fbuf_table = new Object();
			Module.api.fbuf_next_handle = 1;
		}
		for(;;) {
			let h = Module.api.fbuf_next_handle;
			Module.api.fbuf_next_handle++;
			if (Module.api.fbuf_next_handle == 0x80000000)
				Module.api.fbuf_next_handle = 1;
			if (typeof Module.api.fbuf_table[h] == "undefined") {
				return h;
			}
		}
	},
	
	file_buffer_init: function(bs) {
		HEAPU32[bs >> 2] = 0;
		HEAPU32[(bs + 4) >> 2] = 0;
	},

	file_buffer_resize__deps: ['file_buffer_get_new_handle'],
	file_buffer_resize: function(bs, new_size) {
		let h = HEAPU32[bs >> 2];
		let size = HEAPU32[(bs + 4) >> 2];
		
		if (new_size == 0) {
			if (h != 0) {
				delete Module.api.fbuf_table[h];
				h = 0;
			}
		}
		else if (size == 0) {
			h = _file_buffer_get_new_handle();
			Module.api.fbuf_table[h] = new Uint8Array(new_size);
		}
		else if (size != new_size) {
			let data = Module.api.fbuf_table[h];
			let new_data = new Uint8Array(new_size);
			if (new_size > size) {
				new_data.set(data, 0);
			}
			else {
				for (let i = 0; i < new_size; i = (i + 1) | 0) {
					new_data[i] = data[i];
				}
			}
			Module.api.fbuf_table[h] = new_data;
		}

		HEAPU32[bs >> 2] = h;
		HEAPU32[(bs + 4) >> 2] = new_size;
		return 0;
	},
	
	file_buffer_reset: function(bs) {
		_file_buffer_resize(bs, 0);
		_file_buffer_init(bs);
	},
	
	file_buffer_write: function(bs, offset, buf, size) {
		let h = HEAPU32[bs >> 2];
		if (h) {
			let data = Module.api.fbuf_table[h];
			for (let i = 0; i < size; i = (i + 1) | 0) {
				data[offset + i] = HEAPU8[buf + i];
			}
		}
	},
	
	file_buffer_read: function(bs, offset, buf, size) {
		let h = HEAPU32[bs >> 2];
		if (h) {
			let data = Module.api.fbuf_table[h];
			for (let i = 0; i < size; i = (i + 1) | 0) {
				HEAPU8[buf + i] = data[offset + i];
			}
		}
	},

	file_buffer_set: function(bs, offset, val, size) {
		let h = HEAPU32[bs >> 2];
		if (h) {
			let data = Module.api.fbuf_table[h];
			for (let i = 0; i < size; i = (i + 1) | 0) {
				data[offset + i] = val;
			}
		}
	}
});
