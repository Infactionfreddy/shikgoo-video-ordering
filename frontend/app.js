// gemeinsame utilities für customer.js und waiter.js
// alles an window hängen damit die anderen dateien drankommen (kein modul-system)
(function() {
  'use strict';

  var ws = null;
  var attempt = 0;          // reconnect-zähler, treibt den backoff
  var BASE_DELAY = 500;
  var MAX_DELAY = 30000;    // deckel bei 30s, sonst wächst der backoff ins unendliche
  var messageHandler = null;
  var readyHandler = null;
  var toastTimer = null;
  var _urlSuffix = '';      // z.b. ?role=waiter, hängt der aufrufer an die ws-url

  function wsSetStatus(state) {
    var dot = document.getElementById('ws-status');
    if (!dot) return;
    dot.className = 'ws-dot ' + state;
    dot.title = state.charAt(0).toUpperCase() + state.slice(1);
    if (state === 'reconnecting') {
      window.showToast('Reconnecting...');
    } else if (state === 'disconnected') {
      window.showToast('Connection lost. Trying to reconnect.', true); // persistent, bleibt bis wieder da
    }
    // ganze seite dimmen solange nicht connected (css macht den rest)
    document.body.classList.toggle('ws-offline', state !== 'connected');
  }

  // verbindet und reconnectet bei fehler/close automatisch.
  // delay verdoppelt sich pro fehlversuch (+ jitter), siehe onclose
  function wsConnect() {
    var proto = location.protocol.replace('http', 'ws'); // http->ws, https->wss
    ws = new WebSocket(proto + '//' + location.host + '/ws/notifications' + _urlSuffix);

    ws.onopen = function() {
      attempt = 0; // sonst startet der nächste reconnect mit vollem backoff
      wsSetStatus('connected');
      if (typeof readyHandler === 'function') readyHandler();
    };

    ws.onmessage = function(ev) {
      try {
        var msg = JSON.parse(ev.data);
        if (typeof messageHandler === 'function') {
          messageHandler(msg);
        }
      } catch(e) {
        console.warn('[WS] Bad JSON:', ev.data); // kaputte frames nur loggen, nicht crashen
      }
    };

    ws.onclose = function() {
      // erster drop "disconnected", danach "reconnecting", sonst blinkt die ui
      var state = (attempt === 0) ? 'disconnected' : 'reconnecting';
      wsSetStatus(state);
      // ws weg = signaling weg, laufenden call sofort abbauen sonst friert er ein
      if (typeof window.rtcHangup === 'function') window.rtcHangup();
      var jitter = Math.random() * 200; // damit nicht alle clients gleichzeitig reconnecten (thundering herd)
      var delay = Math.min(BASE_DELAY * Math.pow(2, attempt++) + jitter, MAX_DELAY);
      setTimeout(wsConnect, delay);
    };

    ws.onerror = function() {
      ws.close(); // onclose reconnectet eh, sonst doppelt
    };
  }

  // onReady feuert bei JEDEM (re)connect, nicht nur beim ersten
  window.wsInit = function(handler, onReady, urlSuffix) {
    messageHandler = handler;
    readyHandler = onReady || null;
    _urlSuffix = urlSuffix || '';
    wsConnect();
  };

  // wenn die verbindung grad nicht offen ist einfach verschlucken statt werfen,
  // der reconnect regelt das und der aufrufer merkt nix
  window.sendWsMessage = function(obj) {
    if (ws && ws.readyState === WebSocket.OPEN) {
      ws.send(JSON.stringify(obj));
    }
  };

  // persistent=true bleibt stehen bis was drüberschreibt
  // (der "connection lost"-toast soll nicht nach 3s weg sein)
  window.showToast = function(msg, persistent) {
    var container = document.getElementById('toast');
    if (!container) return;
    container.innerHTML = '<div class="toast">' + window.escapeHtml(msg) + '</div>';
    if (!persistent) {
      clearTimeout(toastTimer); // alten timer killen, sonst räumt der den neuen toast zu früh weg
      toastTimer = setTimeout(function() {
        container.innerHTML = '';
      }, 3000);
    }
  };

  // escapen bevor was per innerHTML ins dom geht (xss)
  window.escapeHtml = function(str) {
    return String(str)
      .replace(/&/g, '&amp;')
      .replace(/</g, '&lt;')
      .replace(/>/g, '&gt;')
      .replace(/"/g, '&quot;');
  };

  // server rechnet in cent (kein float-gefrickel), hier nach € umrechnen
  window.formatPrice = function(cents) {
    return '€' + (cents / 100).toFixed(2);
  };

  var STATUS_LABELS = {
    pending:   'Waiting',
    accepted:  'Accepted',
    preparing: 'Preparing',
    ready:     'Ready to Serve',
    delivered: 'Delivered',
    bill:      'Bill Requested',
    paid:      'Paid'
  };

  window.statusLabel = function(status) {
    return STATUS_LABELS[status] || status; // unbekannter status? rohen key zeigen
  };

})();
