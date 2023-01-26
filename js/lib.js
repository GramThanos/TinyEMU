/*
 * JS emulator library
 * 
 * Copyright (c) 2017 Fabrice Bellard
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

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
		// Note: we really send byte values. It would be up to the terminal to support UTF-8
		let str = String.fromCharCode.apply(String, HEAPU8.subarray(buf, buf + len));
		Module.api.terminal.write(str);
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
		let image_data = display.image.data;
		let image_stride = display.width * 4;
		let dst_pos1 = (y * display.width + x) * 4;
		for (let i = 0; i < h; i = (i + 1) | 0) {
			let src = data;
			let dst_pos = dst_pos1;
			for (let j = 0; j < w; j = (j + 1) | 0) {
				let v = HEAPU32[src >> 2];
				image_data[dst_pos] = (v >> 16) & 0xff;
				image_data[dst_pos + 1] = (v >> 8) & 0xff;
				image_data[dst_pos + 2] = v & 0xff;
				image_data[dst_pos + 3] = 0xff; /* XXX: do it once */
				src = (src + 4) | 0;
				dst_pos = (dst_pos + 4) | 0;
			}
			data = (data + stride) | 0;
			dst_pos1 = (dst_pos1 + image_stride) | 0;
		}

		display.ctx.putImageData(display.image, 0, 0, x, y, w, h);
	},

	net_recv_packet: function(bs, buf, len) {
		//console.log('net_recv_packet', bs, buf, len);
		if (Module.api.net) {
			Module.api.net.recvPacket(HEAPU8.subarray(buf, buf + len));
		}
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
