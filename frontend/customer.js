// kunden-seite: menü browsen, bestellen, kellner per video rufen
// läuft alles über den einen websocket, kein fetch() - lan-only
// braucht aus app.js: wsInit, sendWsMessage, showToast, escapeHtml, formatPrice, statusLabel
(function() {
  'use strict';

  // state
  var cart = [];
  var tableNumber = null;
  var currentOrderId = null;
  var pendingOrder   = false; // doppel-tap lock

  // reihenfolge = fortschritt, index treibt den stepper
  var STATUS_SEQUENCE = ['pending', 'accepted', 'preparing', 'ready', 'delivered', 'bill', 'paid'];

  // fallback: keine tischnr in der url, also grid 1-10 anbieten
  function showTableSelect() {
    var tableSection = document.getElementById('table-select');
    var menuScreen = document.getElementById('menu-screen');
    if (tableSection) tableSection.classList.remove('hidden');
    if (menuScreen) menuScreen.classList.add('hidden');

    var grid = document.getElementById('table-grid');
    if (!grid) return;
    grid.innerHTML = '';

    // closure, sonst hat der handler am ende überall i=11
    for (var i = 1; i <= 10; i++) {
      (function(tableNum) {
        var btn = document.createElement('button');
        btn.className = 'table-btn btn-secondary';
        btn.textContent = tableNum;
        btn.setAttribute('aria-label', 'Table ' + tableNum);
        btn.addEventListener('click', function() {
          tableNumber = tableNum;
          sessionStorage.setItem('table_number', String(tableNum));
          history.replaceState(null, '', '/customer/' + tableNum); // url behält den tisch überm reload
          showMenuScreen();
          window.sendWsMessage({ type: 'get_menu' });
          var callBtn = document.getElementById('call-waiter');
          if (callBtn) callBtn.classList.remove('hidden');
        });
        grid.appendChild(btn);
      })(i);
    }
  }

  function showMenuScreen() {
    var tableSection = document.getElementById('table-select');
    var menuScreen = document.getElementById('menu-screen');
    if (tableSection) tableSection.classList.add('hidden');
    if (menuScreen) menuScreen.classList.remove('hidden');
  }

  // menü kommt als menu_data übers ws rein, nicht beim laden
  function renderMenu(items) {
    var grid = document.getElementById('menu-grid');
    if (!grid) return;
    grid.innerHTML = '';
    if (!items || items.length === 0) {
      // leer: ws kaputt oder küche hat nix eingepflegt
      grid.innerHTML = '<p class="subtitle">' +
        window.escapeHtml('Could not load menu. Please refresh or ask staff.') + '</p>';
      return;
    }

    // nach kategorie gruppieren, array-reihenfolge bleibt (server sortiert schon)
    var categories = [];
    var grouped = {};
    items.forEach(function(item) {
      var cat = item.category || 'Sonstiges';
      if (!grouped[cat]) {
        grouped[cat] = [];
        categories.push(cat);
      }
      grouped[cat].push(item);
    });

    categories.forEach(function(cat) {
      var heading = document.createElement('h2');
      heading.className = 'menu-category-heading';
      heading.textContent = cat;
      grid.appendChild(heading);

      var section = document.createElement('div');
      section.className = 'menu-category-grid';
      grouped[cat].forEach(function(item) {
        section.appendChild(buildMenuCard(item));
      });
      grid.appendChild(section);
    });
  }

  // menge lebt im cart-array, die karte selbst hält keinen state
  function buildMenuCard(item) {
    var card = document.createElement('div');
    card.className = 'card menu-item-card';

    var name = document.createElement('h3');
    name.textContent = item.name;
    card.appendChild(name);

    if (item.description) {
      var desc = document.createElement('p');
      desc.className = 'item-description';
      desc.textContent = item.description;
      card.appendChild(desc);
    }

    var cardFooter = document.createElement('div');
    cardFooter.className = 'card-footer';

    var price = document.createElement('div');
    price.className = 'item-price';
    price.textContent = window.formatPrice(item.price);
    cardFooter.appendChild(price);

    var qtyControls = document.createElement('div');
    qtyControls.className = 'qty-controls';

    var minusBtn = document.createElement('button');
    minusBtn.className = 'qty-btn';
    minusBtn.textContent = '−';
    minusBtn.setAttribute('aria-label', 'Remove ' + item.name);

    var qtyDisplay = document.createElement('span');
    qtyDisplay.className = 'qty-display';
    qtyDisplay.textContent = '0';

    var plusBtn = document.createElement('button');
    plusBtn.className = 'qty-btn';
    plusBtn.textContent = '+';
    plusBtn.setAttribute('aria-label', 'Add ' + item.name);

    // cart ist die wahrheit, nicht das dom (sonst driften anzeige und bestellung)
    function getQty() {
      var entry = cart.find(function(c) { return c.id === item.id; });
      return entry ? entry.qty : 0;
    }

    function updateDisplay() {
      qtyDisplay.textContent = String(getQty());
    }

    plusBtn.addEventListener('click', function() {
      var entry = cart.find(function(c) { return c.id === item.id; });
      if (entry) {
        entry.qty++;
      } else {
        cart.push({ id: item.id, name: item.name, price: item.price, qty: 1 });
      }
      updateDisplay();
      updateCartDrawer();
    });

    minusBtn.addEventListener('click', function() {
      var entry = cart.find(function(c) { return c.id === item.id; });
      if (entry && entry.qty > 0) {
        entry.qty--;
        if (entry.qty === 0) {
          // bei 0 ganz raus, sonst müllt der cart voll leerer einträge
          cart = cart.filter(function(c) { return c.id !== item.id; });
        }
      }
      updateDisplay();
      updateCartDrawer();
    });

    qtyControls.appendChild(minusBtn);
    qtyControls.appendChild(qtyDisplay);
    qtyControls.appendChild(plusBtn);
    cardFooter.appendChild(qtyControls);
    card.appendChild(cardFooter);

    return card;
  }

  // drawer nur wenn was drin ist. body-klasse triggert das css
  function updateCartDrawer() {
    var itemCount = cart.reduce(function(sum, c) { return sum + c.qty; }, 0);
    var totalCents = cart.reduce(function(sum, c) { return sum + c.price * c.qty; }, 0); // cent, keine float-fehler

    var summary = document.getElementById('cart-summary');
    if (summary) {
      summary.textContent = itemCount + ' items — ' + window.formatPrice(totalCents);
    }

    var drawer = document.getElementById('cart-drawer');
    if (drawer) {
      if (cart.length > 0) {
        drawer.classList.remove('hidden');
        document.body.classList.add('cart-open'); // css schiebt damit den call-button hoch
      } else {
        drawer.classList.add('hidden');
        document.body.classList.remove('cart-open');
      }
    }
  }

  // schickt nur den ws-request. antwort kommt async als order_placed / order_error
  function submitOrder() {
    if (cart.length === 0 || pendingOrder) return; // leer oder schon unterwegs

    var btn = document.getElementById('place-order');
    if (btn) btn.disabled = true;
    pendingOrder = true;

    window.sendWsMessage({
      type: 'place_order',
      table_number: Number(tableNumber),
      items: cart.map(function(c) {
        return { menu_item_id: c.id, quantity: c.qty };
      })
    });
  }

  // zeichnet die leiste immer komplett neu, kein diffing (billig genug)
  function showOrderStatusBar(status) {
    var section = document.getElementById('order-status-section');
    if (section) section.classList.remove('hidden');

    var bar = document.getElementById('order-status-bar');
    if (!bar) return;

    var delivered = status === 'paid'; // "paid" = ganz durch, alle steps grün
    var currentIndex = STATUS_SEQUENCE.indexOf(status);
    bar.innerHTML = '';
    // alten danke-text weg, sonst hängt er nach jedem redraw doppelt dran
    var existingMsg = bar.parentNode.querySelector('.order-delivered-msg');
    if (existingMsg) existingMsg.parentNode.removeChild(existingMsg);

    STATUS_SEQUENCE.forEach(function(s, index) {
      var step = document.createElement('div');
      step.className = 'status-step';
      if (delivered || index < currentIndex) {
        step.classList.add('done');
      } else if (index === currentIndex) {
        step.classList.add('active');
      }
      var dot = document.createElement('div');
      dot.className = 'step-dot';
      var label = document.createElement('span');
      label.textContent = window.statusLabel(s);
      step.appendChild(dot);
      step.appendChild(label);
      bar.appendChild(step);
    });

    if (delivered) {
      // bezahlt: order-id lokal wegwerfen, sonst zeigt ein reload die alte bestellung
      currentOrderId = null;
      localStorage.removeItem('current_order_id');
      var msg = document.createElement('p');
      msg.className = 'order-delivered-msg';
      msg.textContent = 'Thank you! Payment confirmed. Enjoy your meal!';
      bar.parentNode.appendChild(msg);
    }
  }

  // alles was der server pusht landet hier. webrtc-kram geht weiter an webrtc.js
  function onWsMessage(msg) {
    switch (msg.type) {

      case 'menu_data':
        renderMenu(msg.items);
        break;

      case 'order_placed':
        pendingOrder = false;
        (function() {
          var btn = document.getElementById('place-order');
          if (btn) btn.disabled = false;
        })();
        currentOrderId = msg.order_id;
        localStorage.setItem('current_order_id', String(msg.order_id)); // für reload
        window.showToast("Order placed! We'll start preparing it shortly.");
        cart = []; // korb ist jetzt beim server
        updateCartDrawer();
        showOrderStatusBar('pending');
        break;

      case 'order_error':
        // nur meckern wenn wir grad aktiv bestellt haben, sonst ists ein verwaister fehler
        if (pendingOrder) {
          pendingOrder = false;
          (function() {
            var btn = document.getElementById('place-order');
            if (btn) btn.disabled = false;
          })();
          window.showToast('Order could not be sent. Check your connection and try again.');
        } else {
          // server kennt die order nicht mehr, stale id raus
          localStorage.removeItem('current_order_id');
        }
        break;

      case 'order_data':
        // antwort auf get_order/get_table_order, status nach reload wiederherstellen
        if (msg.id && msg.status) {
          currentOrderId = msg.id;
          localStorage.setItem('current_order_id', String(msg.id));
          showOrderStatusBar(msg.status);
        }
        break;

      case 'order_update':
        // broadcast an alle am tisch, nur reagieren wenns wirklich unsere order ist
        if (msg.order_id === currentOrderId) {
          showOrderStatusBar(msg.status);
          window.showToast('Order update: ' + window.statusLabel(msg.status));
        }
        break;

      case 'call_ring':
      case 'call_reject':
      case 'call_busy':
      case 'call_end':
      case 'webrtc_offer':
      case 'webrtc_answer':
      case 'webrtc_ice':
        // reines signaling, customer.js reicht nur durch, webrtc.js macht die arbeit
        if (typeof window.rtcHandleSignal === 'function') window.rtcHandleSignal(msg);
        break;

      case 'call_accept':
        // erst panel zeigen, DANN webrtc starten - das video-element muss existieren
        // bevor der stream kommt (safari sonst kurz schwarz / motzt bei srcObject auf hidden)
        (function() {
          var css = document.getElementById('call-status-section');
          if (css) css.classList.add('hidden');
          var cap = document.getElementById('call-active-panel');
          if (cap) cap.classList.remove('hidden');
          document.body.classList.add('call-active');
          var hb = document.getElementById('hangup-btn');
          if (hb) hb.focus(); // fokus aufs auflegen
        })();
        if (typeof window.rtcHandleSignal === 'function') window.rtcHandleSignal(msg);
        break;

    }
  }

  // läuft bei jedem (re)connect. alles frisch holen, lokaler state ist evtl alt
  function onWsReady() {
    if (tableNumber !== null) {
      window.sendWsMessage({ type: 'get_menu' });
      // per tisch fragen klappt auch wenn der browser-cache leer ist
      window.sendWsMessage({ type: 'get_table_order', table_number: Number(tableNumber) });
    }
    // zusätzlich per id, falls localStorage noch eine kennt (fallback für alte server ohne get_table_order)
    var storedOid = localStorage.getItem('current_order_id');
    if (storedOid) {
      window.sendWsMessage({ type: 'get_order', order_id: Number(storedOid) });
    }
  }

  // init
  window.addEventListener('DOMContentLoaded', function() {
    // tischnr aus der url: /customer/3 = tisch 3, das steht im qr-code am tisch
    var pathParts = window.location.pathname.split('/');
    var urlTable = pathParts.length >= 3 ? parseInt(pathParts[2], 10) : NaN;

    if (!isNaN(urlTable) && urlTable >= 1 && urlTable <= 20) {
      tableNumber = urlTable;
      sessionStorage.setItem('table_number', String(urlTable));
      showMenuScreen();
      var callBtn = document.getElementById('call-waiter');
      if (callBtn) callBtn.classList.remove('hidden');
    } else {
      // keine/ungültige nummer, also erst tisch wählen lassen
      sessionStorage.removeItem('table_number');
      showTableSelect();
    }

    // ?table= hängt dran, damit der server den ws-client dem tisch zuordnet
    window.wsInit(onWsMessage, onWsReady, tableNumber ? '?table=' + tableNumber : '');
    if (typeof window.rtcInit === 'function') window.rtcInit();

    var placeOrderBtn = document.getElementById('place-order');
    if (placeOrderBtn) {
      placeOrderBtn.addEventListener('click', submitOrder);
    }

    var callWaiterBtn = document.getElementById('call-waiter');
    if (callWaiterBtn) {
      callWaiterBtn.addEventListener('click', function() {
        window.sendWsMessage({ type: 'call_ring', table_number: Number(tableNumber) });
        callWaiterBtn.classList.add('hidden'); // weg, sonst klingelt man 3x während es schon klingelt
        var statusSection = document.getElementById('call-status-section');
        if (statusSection) statusSection.classList.remove('hidden');
      });
    }

    var cancelCallBtn = document.getElementById('cancel-call');
    if (cancelCallBtn) {
      cancelCallBtn.addEventListener('click', function() {
        window.sendWsMessage({ type: 'call_end', table_number: Number(tableNumber) });
        window.rtcHangup();
      });
    }

    // erst call_end übers ws, DANN rtcHangup - sonst ist die peerconnection tot
    // und der kellner kriegt das end nie. table_number muss mit, sonst findet der server den call nicht
    var hangupBtn = document.getElementById('hangup-btn');
    if (hangupBtn) {
      hangupBtn.addEventListener('click', function() {
        window.sendWsMessage({ type: 'call_end', table_number: Number(tableNumber) });
        window.rtcHangup();
      });
    }
  });

})();
