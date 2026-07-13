/* webrtc.js - die ganze call-engine
 * IIFE, haengt rtcInit / rtcHandleSignal / rtcHangup ans window.
 * ladereihenfolge: app.js, dann das hier, dann customer.js bzw waiter.js
 */
(function() {
  'use strict';

  // closure-state
  var _pendingCandidates  = [];   // ice die reinkommt bevor der pc steht -> puffern
  var _peerConnection     = null;
  var _currentCallerTable = null; // table_number aus call_ring (kellner-seite)
  var _prevFocus          = null; // fokus vorm overlay, beim schliessen zurueck
  var _localStream        = null; // eigener cam/mic stream
  var _pipCleanup         = null; // cleanup fuers pip-drag
  var _callWindowCleanup  = null; // cleanup fuers fenster-drag
  var _callResizeCleanup  = null; // cleanup fuer grip + pinch

  // gemeinsame drag-mechanik, es5 (kein arrow, kein let/const)

  /* drag. el wird bewegt, handle kriegt die listener
   * (bei pip beides dasselbe, beim fenster der header).
   * clamp gegen el's aktuelle groesse, nicht hardcoded - laeuft so fuer pip UND fenster.
   * gibt cleanup zurueck (listener weg + cursor reset), inline-styles raeumt der caller.
   */
  function _makeDraggable(el, handle, container) {
    var startX = 0, startY = 0, startLeft = 0, startTop = 0;
    var maxX = 0, maxY = 0;

    function _beginDrag(clientX, clientY) {
      startX = clientX;
      startY = clientY;
      if (container) {
        // pip: el ist absolute im container. offsetLeft/Top passt genau zu style.left/top.
        // clamp gegen die innenmasse vom fenster, nicht viewport.
        startLeft = el.offsetLeft;
        startTop  = el.offsetTop;
        maxX = container.clientWidth  - el.offsetWidth;
        maxY = container.clientHeight - el.offsetHeight;
      } else {
        // fenster: fixed, also viewport-koordinaten.
        var r = el.getBoundingClientRect();
        startLeft = r.left;
        startTop  = r.top;
        maxX = window.innerWidth  - r.width;
        maxY = window.innerHeight - r.height;
      }
      el.style.left   = startLeft + 'px';
      el.style.top    = startTop  + 'px';
      el.style.right  = '';
      el.style.bottom = '';
      handle.style.cursor = 'grabbing';
    }

    function _moveDrag(clientX, clientY) {
      var x = startLeft + (clientX - startX);
      var y = startTop  + (clientY - startY);
      x = Math.max(0, Math.min(x, maxX));
      y = Math.max(0, Math.min(y, maxY));
      el.style.left = x + 'px';
      el.style.top  = y + 'px';
    }

    function _endDrag() {
      handle.style.cursor = 'grab';
      document.removeEventListener('mousemove', _onMouseMove);
      document.removeEventListener('mouseup',   _onMouseUp);
    }

    function _onMouseDown(e) {
      if (e.target.closest && e.target.closest('button')) { return; }
      _beginDrag(e.clientX, e.clientY);
      document.addEventListener('mousemove', _onMouseMove);
      document.addEventListener('mouseup',   _onMouseUp);
      e.preventDefault();
    }

    function _onMouseMove(e) { _moveDrag(e.clientX, e.clientY); }
    function _onMouseUp()    { _endDrag(); }

    function _onTouchStart(e) {
      if (e.touches.length !== 1) { return; } // zwei finger = pinch, kein drag
      if (e.target.closest && e.target.closest('button')) { return; }
      _beginDrag(e.touches[0].clientX, e.touches[0].clientY);
      e.preventDefault();
    }

    function _onTouchMove(e) {
      if (e.touches.length !== 1) { return; } // pinch macht das panel
      _moveDrag(e.touches[0].clientX, e.touches[0].clientY);
      e.preventDefault();
    }

    handle.addEventListener('mousedown',  _onMouseDown);
    handle.addEventListener('touchstart', _onTouchStart, { passive: false });
    handle.addEventListener('touchmove',  _onTouchMove,  { passive: false });

    return function() {
      handle.removeEventListener('mousedown',  _onMouseDown);
      handle.removeEventListener('touchstart', _onTouchStart);
      handle.removeEventListener('touchmove',  _onTouchMove);
      document.removeEventListener('mousemove', _onMouseMove);
      document.removeEventListener('mouseup',   _onMouseUp);
      handle.style.cursor = '';
    };
  }

  /* eigenes bild als pip ins remote-bild rein, absolute im call-panel.
   * start oben rechts (unten mitte ist der end-call button).
   * drag auf die panel-innenmasse geclampt, pip kann also nicht raus.
   */
  function _initPip(wrap) {
    if (!wrap) { return; }
    var container = wrap.parentElement; // das call-panel
    wrap.style.position = 'absolute';
    wrap.style.width    = '33%';
    wrap.style.height   = '33%';
    wrap.style.top      = '10px';
    wrap.style.right    = '10px';
    wrap.style.left     = '';
    wrap.style.bottom   = '';
    wrap.style.zIndex   = '20';
    wrap.classList.add('pip-active');

    var _dragCleanup = _makeDraggable(wrap, wrap, container);

    _pipCleanup = function() {
      _dragCleanup();
      wrap.classList.remove('pip-active');
      wrap.style.position = '';
      wrap.style.left     = '';
      wrap.style.top      = '';
      wrap.style.right    = '';
      wrap.style.bottom   = '';
      wrap.style.width    = '';
      wrap.style.height   = '';
      wrap.style.zIndex   = '';
      wrap.style.cursor   = '';
      _pipCleanup = null;
    };
  }

  // pip-handler weg + styles zurueck, erster schritt im hangup
  function _cleanupPip() {
    if (_pipCleanup) { _pipCleanup(); }
  }

  /* fenster am .call-panel-header draggable. groesse/radius/shadow macht css,
   * hier nur der drag + beim cleanup left/top zurueck.
   */
  function _initCallWindow(panel) {
    if (!panel) { return; }
    var handle = panel.querySelector('.call-panel-header') || panel;
    var _dragCleanup = _makeDraggable(panel, handle);
    _callWindowCleanup = function() {
      _dragCleanup();
      panel.style.left = '';
      panel.style.top  = '';
      _callWindowCleanup = null;
    };
  }

  function _cleanupCallWindow() {
    if (_callWindowCleanup) { _callWindowCleanup(); }
  }

  /* fenster skalierbar: maus per griff unten rechts, touch per zwei-finger-pinch.
   * ratio fix 4:3 sonst verzerrts. pip + videos skalieren prozentual mit.
   * groesse gegen MIN und viewport geclampt.
   */
  function _initResize(panel) {
    if (!panel) { return; }
    var MIN_W = 220;          // drunter unbrauchbar (buttons/header)
    var MAX_W = 720;          // hartes max
    var RATIO = 270 / 360;    // 4:3 wie die css-startgroesse

    // breite anwenden, gegen min+viewport clampen, hoehe aus ratio
    function _applySize(w) {
      var maxW = Math.min(MAX_W, window.innerWidth - 20, (window.innerHeight - 20) / RATIO);
      w = Math.max(MIN_W, Math.min(w, maxW));
      panel.style.width  = w + 'px';
      panel.style.height = Math.round(w * RATIO) + 'px';
    }

    // maus: griff unten rechts
    var grip = document.createElement('div');
    grip.className = 'call-resize-grip';
    grip.setAttribute('aria-hidden', 'true');
    panel.appendChild(grip);

    var startX = 0, startW = 0;
    function _onGripDown(e) {
      startX = e.clientX;
      startW = panel.offsetWidth;
      document.addEventListener('mousemove', _onGripMove);
      document.addEventListener('mouseup',   _onGripUp);
      e.preventDefault();
      e.stopPropagation();    // sonst wirds als fenster-drag gelesen
    }
    function _onGripMove(e) { _applySize(startW + (e.clientX - startX)); }
    function _onGripUp() {
      document.removeEventListener('mousemove', _onGripMove);
      document.removeEventListener('mouseup',   _onGripUp);
    }
    grip.addEventListener('mousedown', _onGripDown);

    // touch: zwei-finger-pinch aufs panel
    var pinchStartDist = 0, pinchStartW = 0;
    function _dist(touches) {
      var dx = touches[0].clientX - touches[1].clientX;
      var dy = touches[0].clientY - touches[1].clientY;
      return Math.sqrt(dx * dx + dy * dy);
    }
    function _onTouchStart(e) {
      if (e.touches.length === 2) {
        pinchStartDist = _dist(e.touches);
        pinchStartW    = panel.offsetWidth;
        e.preventDefault();
      }
    }
    function _onTouchMove(e) {
      if (e.touches.length === 2 && pinchStartDist > 0) {
        _applySize(pinchStartW * (_dist(e.touches) / pinchStartDist));
        e.preventDefault();
      }
    }
    function _onTouchEnd(e) {
      if (e.touches.length < 2) { pinchStartDist = 0; }
    }
    panel.addEventListener('touchstart', _onTouchStart, { passive: false });
    panel.addEventListener('touchmove',  _onTouchMove,  { passive: false });
    panel.addEventListener('touchend',   _onTouchEnd);

    _callResizeCleanup = function() {
      grip.removeEventListener('mousedown', _onGripDown);
      document.removeEventListener('mousemove', _onGripMove);
      document.removeEventListener('mouseup',   _onGripUp);
      panel.removeEventListener('touchstart', _onTouchStart);
      panel.removeEventListener('touchmove',  _onTouchMove);
      panel.removeEventListener('touchend',   _onTouchEnd);
      if (grip.parentNode) { grip.parentNode.removeChild(grip); }
      panel.style.width  = '';
      panel.style.height = '';
      _callResizeCleanup = null;
    };
  }

  function _cleanupResize() {
    if (_callResizeCleanup) { _callResizeCleanup(); }
  }

  // private helper, es5 only (kein arrow, kein let/const)

  /* pc bauen. iceServers:[] weil lan-only, kein stun/turn noetig.
   * haengt onicecandidate / ontrack / iceconnectionstatechange dran.
   */
  function _createPeerConnection() {
    var pc = new RTCPeerConnection({ iceServers: [] });

    pc.onicecandidate = function(event) {
      if (event.candidate) {
        window.sendWsMessage({ type: 'webrtc_ice', candidate: event.candidate });
      }
    };

    pc.ontrack = function(event) {
      var remoteStream;
      if (event.streams && event.streams[0]) {
        remoteStream = event.streams[0];
      } else {
        // fallback wenn kein stream mitkommt: track selbst wrappen
        remoteStream = new MediaStream([event.track]);
      }
      var remoteVid = document.getElementById('remote-video') ||
                      document.getElementById('waiter-remote-video');
      if (remoteVid) {
        remoteVid.autoplay = true;
        remoteVid.srcObject = remoteStream;
        remoteVid.play().catch(function() {});
        // placeholder erst weg wenn das video WIRKLICH laeuft
        remoteVid.onplaying = function() {
          var placeholder = document.getElementById('remote-placeholder') ||
                            document.getElementById('waiter-remote-placeholder');
          if (placeholder) placeholder.classList.add('hidden');
          remoteVid.onplaying = null; // nur einmal
        };
      }
    };

    pc.oniceconnectionstatechange = function() {
      if (pc.iceConnectionState === 'failed') {
        // erst call_end ueber WS, dann hangup (hangup macht nur UI)
        window.sendWsMessage({ type: 'call_end' });
        window.rtcHangup();
        window.showToast('Connection failed — try again.');
      }
    };

    _peerConnection = pc;
    return pc;
  }

  // media-constraints, fuer beide seiten gleich
  var _MEDIA_CONSTRAINTS = {
    video: {
      width:     { ideal: 640 },
      height:    { ideal: 480 },
      frameRate: { ideal: 15, max: 15 }   // mehr als 15fps macht dem pi die cpu dicht
    },
    audio: {
      echoCancellation: true,
      noiseSuppression: true,
      autoGainControl:  true
    }
  };

  /* getUserMedia mit retry bei NotReadableError.
   * firefox/windows haelt die kamera nach reload/vorigem call noch belegt und gibt
   * sie erst verzoegert frei ("Failed to allocate videosource"). also bis 5x mit
   * 500ms pause. andere fehler (permission, kein geraet) fliegen sofort weiter.
   */
  function _gumWithRetry(constraints, retriesLeft) {
    return navigator.mediaDevices.getUserMedia(constraints)
      .catch(function(err) {
        if (err && err.name === 'NotReadableError' && retriesLeft > 0) {
          return new Promise(function(resolve) { setTimeout(resolve, 500); })
            .then(function() { return _gumWithRetry(constraints, retriesLeft - 1); });
        }
        throw err;
      });
  }

  /* lokalen stream holen. erst video+audio mit retry. klappt die kamera gar nicht
   * (NotReadableError), fallback auf audio-only - call kommt trotzdem zustande, halt
   * ohne eigenes bild. rest vom flow laeuft mit reinem audio-stream genauso.
   * andere fehler fliegen weiter -> _showMediaError.
   */
  function _getUserMedia() {
    return _gumWithRetry(_MEDIA_CONSTRAINTS, 5)
      .catch(function(err) {
        if (err && (err.name === 'NotReadableError' || err.name === 'AbortError')) {
          if (window.showToast) {
            window.showToast('Camera unavailable — connecting with audio only.');
          }
          return navigator.mediaDevices.getUserMedia({
            audio: _MEDIA_CONSTRAINTS.audio,
            video: false
          });
        }
        throw err;
      });
  }

  /* offerer-seite, laeuft bei call_accept (also bei dem der den call_ring geschickt hat).
   * gum -> addTrack -> createOffer -> setLocalDescription -> webrtc_offer raus.
   */
  function _startOffer() {
    _getUserMedia()
    .then(function(stream) {
      _localStream = stream;
      var localVid = document.getElementById('local-video') ||
                     document.getElementById('waiter-local-video');
      if (localVid) { localVid.srcObject = stream; }
      if (localVid) { _initPip(localVid.parentElement); }
      var _panel = document.getElementById('call-active-panel') || document.getElementById('waiter-call-active');
      _initCallWindow(_panel);
      _initResize(_panel);

      var pc = _createPeerConnection();
      stream.getTracks().forEach(function(track) { pc.addTrack(track, stream); });

      return pc.createOffer();
    })
    .then(function(offer) {
      if (!_peerConnection) { return; }
      return _peerConnection.setLocalDescription(offer);
    })
    .then(function() {
      if (!_peerConnection) { return; }
      window.sendWsMessage({ type: 'webrtc_offer', sdp: _peerConnection.localDescription });
    })
    .catch(function(err) {
      // erst WS raus, dann hangup (hangup ist nur UI)
      window.sendWsMessage({ type: 'client_error', where: 'startOffer', name: err.name, message: String(err.message || '') });
      window.sendWsMessage({ type: 'call_end' });
      window.rtcHangup();
      _showMediaError(err.name);
    });
  }

  /* answerer-seite, laeuft bei webrtc_offer.
   * gum -> addTrack -> setRemoteDescription -> ice drainen -> createAnswer -> answer raus.
   */
  function _handleOffer(msg) {
    _getUserMedia()
    .then(function(stream) {
      _localStream = stream;
      var localVid = document.getElementById('waiter-local-video') ||
                     document.getElementById('local-video');
      if (localVid) { localVid.srcObject = stream; }
      if (localVid) { _initPip(localVid.parentElement); }
      var _panel = document.getElementById('call-active-panel') || document.getElementById('waiter-call-active');
      _initCallWindow(_panel);
      _initResize(_panel);

      var pc = _createPeerConnection();
      stream.getTracks().forEach(function(track) { pc.addTrack(track, stream); });

      return pc.setRemoteDescription(new RTCSessionDescription(msg.sdp));
    })
    .then(function() {
      if (!_peerConnection) { return; }
      _drainCandidates(); // drainen VOR createAnswer
      return _peerConnection.createAnswer();
    })
    .then(function(answer) {
      if (!_peerConnection) { return; }
      return _peerConnection.setLocalDescription(answer);
    })
    .then(function() {
      if (!_peerConnection) { return; }
      window.sendWsMessage({ type: 'webrtc_answer', sdp: _peerConnection.localDescription });
    })
    .catch(function(err) {
      // erst WS, dann hangup
      window.sendWsMessage({ type: 'client_error', where: 'handleOffer', name: err.name, message: String(err.message || '') });
      window.sendWsMessage({ type: 'call_end' });
      window.rtcHangup();
      _showMediaError(err.name);
    });
  }

  // webrtc_answer auf der offerer-seite. guard falls der pc schon zu ist
  function _handleAnswer(msg) {
    if (!_peerConnection) { return; }
    _peerConnection.setRemoteDescription(new RTCSessionDescription(msg.sdp))
      .then(_drainCandidates)
      .catch(function(e) {
        console.warn('[RTC] setRemoteDescription failed:', e.name);
      });
  }

  /* gepufferte ice nach setRemoteDescription anwenden.
   * buffer erst slicen + leeren, dann anwenden (sonst race).
   */
  function _drainCandidates() {
    if (!_peerConnection) { return; }
    var toApply = _pendingCandidates.slice();
    _pendingCandidates = [];
    toApply.forEach(function(candidate) {
      _peerConnection.addIceCandidate(candidate).catch(function(e) {
        console.warn('[RTC] addIceCandidate failed:', e.name);
      });
    });
  }

  /* zeigt das #media-error banner. NUR DOM, kein hangup/WS - das macht der caller.
   * ueberall null-geguarded weil das hier auf beiden seiten laeuft.
   */
  function _showMediaError(errName) {
    var banner = document.getElementById('media-error');
    if (banner) { banner.classList.remove('hidden'); }

    var heading = document.querySelector('.media-error-heading');
    var body    = document.querySelector('.media-error-body');
    var h, b;

    if (errName === 'NotAllowedError' || errName === 'PermissionDeniedError' ||
        errName === 'SecurityError') {
      // user hat verweigert (oder browser blockt)
      h = 'Camera access denied';
      b = 'To use video calling, allow camera and microphone access in your ' +
          'browser settings, then reload the page.';
    } else if (errName === 'NotReadableError' || errName === 'AbortError' ||
               errName === 'TrackStartError') {
      // geraet da aber nicht startbar, meist von anderer app/tab belegt.
      // genau der windows/firefox "Failed to allocate videosource" fall.
      h = 'Camera could not be started';
      b = 'Your camera or microphone is already in use by another app or ' +
          'browser tab. Close it (e.g. Zoom, Teams, the Camera app, other tabs), ' +
          'then start the call again.';
    } else if (errName === 'NotFoundError' || errName === 'DevicesNotFoundError' ||
               errName === 'OverconstrainedError') {
      // kein passendes geraet
      h = 'No camera found';
      b = 'No camera or microphone was detected. Connect one and reload the page.';
    } else {
      // NotSupportedError oder unbekannt
      h = 'Video calling not supported';
      b = 'Your browser does not support video calling. Try Chrome or Firefox.';
    }

    if (heading) { heading.textContent = h; }
    if (body)    { body.textContent = b; }
  }

  /* mute-button helfer. _setMuteLabel spiegelt den zustand in text, aria-pressed
   * und .is-muted (rot). null-guard weil beide seiten.
   */
  function _muteBtn() {
    return document.getElementById('mute-btn') ||
           document.getElementById('waiter-mute-btn');
  }
  function _setMuteLabel(muted) {
    var btn = _muteBtn();
    if (!btn) { return; }
    btn.textContent = muted ? 'Unmute' : 'Mute';
    btn.setAttribute('aria-pressed', muted ? 'true' : 'false');
    btn.setAttribute('aria-label', muted ? 'Unmute microphone' : 'Mute microphone');
    if (muted) { btn.classList.add('is-muted'); }
    else       { btn.classList.remove('is-muted'); }
  }

  /* rtcInit - haengt die #call-accept / #call-reject buttons an.
   * wird aus customer.js/waiter.js nach wsInit() gerufen.
   * ruft NICHT selbst wsInit (sonst kill ich den page-handler).
   * #cancel-call bleibt in customer.js (braucht die tableNumber closure).
   */
  window.rtcInit = function() {
    var acceptBtn = document.getElementById('call-accept');
    if (acceptBtn) {
      acceptBtn.addEventListener('click', function() {
        window.sendWsMessage({ type: 'call_accept' });
        var overlay = document.getElementById('call-overlay');
        if (overlay) overlay.classList.add('hidden');
        // server schickt call_accept nur an den initiator, nicht zurueck an den acceptor.
        // acceptor muss sein panel also selbst zeigen, es kommt kein event zurueck.
        var wca = document.getElementById('waiter-call-active');
        if (wca) {
          wca.classList.remove('hidden');
          var heading = wca.querySelector('.call-panel-heading');
          if (heading && _currentCallerTable) {
            heading.textContent = 'In Call — Table ' + _currentCallerTable;
          }
          var whb = document.getElementById('waiter-hangup-btn');
          if (whb) whb.focus();
        }
        var css = document.getElementById('call-status-section');
        if (css) css.classList.add('hidden');
        var cap = document.getElementById('call-active-panel');
        if (cap) {
          cap.classList.remove('hidden');
          document.body.classList.add('call-active');
          var hb = document.getElementById('hangup-btn');
          if (hb) hb.focus();
        }
      });
    }

    var rejectBtn = document.getElementById('call-reject');
    if (rejectBtn) {
      rejectBtn.addEventListener('click', function() {
        window.sendWsMessage({ type: 'call_reject' });
        var overlay = document.getElementById('call-overlay');
        if (overlay) overlay.classList.add('hidden');
      });
    }

    // mute: eigene audio-tracks aus, rein lokal. kein WS noetig,
    // track.enabled=false schickt einfach stille.
    var muteBtn = _muteBtn();
    if (muteBtn) {
      muteBtn.addEventListener('click', function() {
        if (!_localStream) { return; }
        var tracks = _localStream.getAudioTracks();
        if (!tracks.length) { return; } // z.b. wenn gar kein mikro da
        var enable = !tracks[0].enabled;               // toggeln
        tracks.forEach(function(t) { t.enabled = enable; });
        _setMuteLabel(!enable);                         // muted = nicht enabled
      });
    }

    // safari-only: kamera-permission beim laden vorwaermen.
    // ios safari will ne user-geste fuer getUserMedia, der echte call_accept kommt
    // aber aus ner WS-nachricht -> keine geste. drum vorab holen.
    // vorsicht: NUR ios. auf desktop (windows/firefox) belegt der frueh-zugriff das
    // geraet und der spaetere call scheitert dann mit NotReadableError.
    var _ua = navigator.userAgent || '';
    var _isIOS = /iPad|iPhone|iPod/.test(_ua) ||
                 (/Macintosh/.test(_ua) && navigator.maxTouchPoints > 1); // ipados tarnt sich als mac
    if (_isIOS && navigator.mediaDevices && navigator.mediaDevices.getUserMedia) {
      navigator.mediaDevices.getUserMedia({ audio: true, video: true })
        .then(function(s) { s.getTracks().forEach(function(t) { t.stop(); }); })
        .catch(function() {}); // egal, wenn abgelehnt fangen wirs beim echten call ab
    }
  };

  /* rtcHandleSignal - routet reinkommende WS-signale auf UI-aktionen.
   * kommt aus onWsMessage in customer.js/waiter.js.
   * alles null-geguarded, elemente der einen seite gibts auf der anderen nicht.
   */
  window.rtcHandleSignal = function(msg) {
    switch (msg.type) {

      case 'call_ring':
        // beide seiten: eingehenden-call overlay zeigen.
        // wo #call-overlay fehlt ist das dank null-guard einfach ein no-op.
        _currentCallerTable = msg.table_number; // aus dem ring, brauch ich fuers heading
        var overlay = document.getElementById('call-overlay');
        if (overlay) overlay.classList.remove('hidden');
        var tableEl = overlay ? overlay.querySelector('p.call-table') : null;
        if (tableEl) tableEl.textContent = 'Table ' + msg.table_number; // textContent, nicht innerHTML (xss)
        _prevFocus = document.activeElement;
        var acceptBtn = document.getElementById('call-accept');
        if (acceptBtn) acceptBtn.focus();
        break;

      case 'call_accept':
        // offer-flow starten, laeuft bei dem der den call_ring geschickt hat
        _startOffer();
        break;

      case 'call_reject':
        // kunde: kellner hat abgelehnt
        window.rtcHangup();
        window.showToast('Waiter is busy right now.');
        break;

      case 'call_busy':
        // kunde: kellner ist schon in nem call
        window.rtcHangup();
        window.showToast('Waiter is busy — please try again in a moment.');
        break;

      case 'call_end':
        // toast nur auf kundenseite (sichtbare status-section = kunde war im call)
        var statusSection = document.getElementById('call-status-section');
        if (statusSection && !statusSection.classList.contains('hidden')) {
          window.showToast('The waiter ended the call.');
        }
        window.rtcHangup();
        break;

      case 'webrtc_offer':
        // answer-path
        _handleOffer(msg);
        break;

      case 'webrtc_answer':
        // offerer kriegt die answer
        _handleAnswer(msg);
        break;

      case 'webrtc_ice':
        // wenn remoteDescription steht direkt adden, sonst puffern
        if (_peerConnection && _peerConnection.remoteDescription) {
          _peerConnection.addIceCandidate(msg.candidate).catch(function(e) {
            console.warn('[RTC] addIceCandidate failed:', e.name);
          });
        } else {
          _pendingCandidates.push(msg.candidate);
        }
        break;
    }
  };

  /* rtcHangup - reines UI-teardown, schickt SELBST keine WS-nachricht.
   * aufrufer: call_reject/busy/end, ws.onclose in app.js, der #cancel-call handler
   * und die gum/ice fehlerpfade (die schicken WS selber).
   */
  window.rtcHangup = function() {
    // erst pip/fenster/resize handler weg, sonst haengen listener nach dem call rum
    _cleanupPip();
    _cleanupCallWindow();
    _cleanupResize();

    // medien-teardown vor den UI-resets

    // pc zu + alle lokalen tracks stoppen
    if (_peerConnection) {
      _peerConnection.close();
      _peerConnection = null;
    }
    if (_localStream) {
      _localStream.getTracks().forEach(function(t) { t.stop(); });
      _localStream = null;
    }

    // srcObject auf allen vier videos raus
    var videoIds = ['local-video', 'remote-video', 'waiter-local-video', 'waiter-remote-video'];
    videoIds.forEach(function(id) {
      var el = document.getElementById(id);
      if (el) { el.srcObject = null; }
    });

    // placeholder wieder zeigen
    var rp = document.getElementById('remote-placeholder');
    if (rp) { rp.classList.remove('hidden'); }
    var wp = document.getElementById('waiter-remote-placeholder');
    if (wp) { wp.classList.remove('hidden'); }

    // aktive call-panels verstecken
    var cap = document.getElementById('call-active-panel');
    if (cap) {
      cap.classList.add('hidden');
      document.body.classList.remove('call-active');
    }
    var wca = document.getElementById('waiter-call-active');
    if (wca) { wca.classList.add('hidden'); }

    // UI zuruecksetzen

    // kunde
    var statusSection = document.getElementById('call-status-section');
    if (statusSection) { statusSection.classList.add('hidden'); }
    var callBtn = document.getElementById('call-waiter');
    if (callBtn) { callBtn.classList.remove('hidden'); }

    // kellner
    var overlay = document.getElementById('call-overlay');
    if (overlay) { overlay.classList.add('hidden'); }

    // fokus zurueck wo er vorm overlay war (a11y)
    if (_prevFocus && typeof _prevFocus.focus === 'function') {
      _prevFocus.focus();
    }
    _prevFocus = null;

    // mute reset, naechster call startet wieder laut
    _setMuteLabel(false);

    // state clearen
    _currentCallerTable = null;
    _pendingCandidates  = [];
  };

})();
