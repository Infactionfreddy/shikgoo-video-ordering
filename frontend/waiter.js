(function() {
'use strict';
// kellner-dashboard: bestellqueue + eingehende calls, alles über ws
// einzige ausnahme: /names.json wird direkt gefetcht (statische datei)
// hängt an app.js (wsInit, sendWsMessage, showToast, escapeHtml, formatPrice, statusLabel)

var currentModalOrderId = null; // offenes modal
var pendingModalOrder   = null; // auf welches order_data wir warten
var menuMap = {};               // id -> {name, price}
var _activeCallTable    = null; // call_accept sendet die tischnr nicht mit, drum hier merken

var NEXT_STATUS = {
  pending:   { next: 'accepted',  label: 'Accept Order' },
  accepted:  { next: 'preparing', label: 'Mark as Preparing' },
  preparing: { next: 'ready',     label: 'Mark as Ready' },
  ready:     { next: 'delivered', label: 'Mark as Delivered' },
  delivered: { next: 'bill',      label: 'Send Bill' },
  bill:      { next: 'paid',      label: 'Mark as Paid' }
};

function loadMenu() {
  window.sendWsMessage({ type: 'get_menu' });

  // names.json = optionale namens-overrides (z.b. deutsche übersetzungen)
  // direkter fetch ok, statische datei und kein api
  fetch('/names.json')
    .then(function(res) { return res.ok ? res.json() : {}; })
    .then(function(names) {
      Object.keys(names).forEach(function(id) {
        var numId = parseInt(id, 10);
        if (menuMap[numId]) menuMap[numId].name = names[id];
        else menuMap[numId] = { id: numId, name: names[id], price: 0 };
      });
    })
    .catch(function() {}); // fehlt halt, egal
}

function loadQueue() {
  window.sendWsMessage({ type: 'get_queue' });
}

function renderQueue(orders) {
  var queue = document.getElementById('order-queue');
  var emptyState = document.getElementById('empty-state');

  orders.sort(function(a, b) { return b.id - a.id; }); // neueste oben

  if (orders.length === 0) {
    var cards = queue.querySelectorAll('.order-card');
    cards.forEach(function(card) { card.remove(); });
    emptyState.classList.remove('hidden');
    return;
  }

  emptyState.classList.add('hidden');

  // einfach alles neu bauen statt karten zu diffen, queue ist eh klein
  var oldCards = queue.querySelectorAll('.order-card');
  oldCards.forEach(function(card) { card.remove(); });

  orders.forEach(function(order) {
    var card = buildOrderCard(order);
    queue.appendChild(card);
  });
}

function buildOrderCard(order) {
  var card = document.createElement('div');
  card.className = 'card order-card';
  card.setAttribute('data-order-id', order.id);
  card.setAttribute('data-table', order.table_number); // fürs call-table delegation unten

  var header = document.createElement('div');
  header.className = 'order-card-header';

  var tableLabel = document.createElement('span');
  tableLabel.className = 'order-card-table';
  tableLabel.textContent = 'Table ' + order.table_number;

  var badge = document.createElement('span');
  badge.className = 'badge badge-' + order.status;
  badge.textContent = window.statusLabel(order.status);

  header.appendChild(tableLabel);
  header.appendChild(badge);
  card.appendChild(header);

  var meta = document.createElement('div');
  meta.className = 'order-card-meta';
  var itemCount = order.item_count || (Array.isArray(order.items) ? order.items.length : 0);
  var timeStr = order.timestamp
    ? new Date(
        typeof order.timestamp === 'number'
          ? order.timestamp * 1000  // unix-sekunden, Date will millis
          : order.timestamp
      ).toLocaleTimeString()
    : '';
  meta.textContent = itemCount + ' item' + (itemCount !== 1 ? 's' : '') +
    (timeStr ? ' · ' + timeStr : '');
  card.appendChild(meta);

  if (order.customer_name && order.customer_name.trim() !== '') {
    var nameEl = document.createElement('span');
    nameEl.className = 'order-card-customer';
    nameEl.textContent = window.escapeHtml(order.customer_name); // kunden-input, escapen
    card.appendChild(nameEl);
  }

  var btnGroup = document.createElement('div');
  btnGroup.className = 'status-btn-group';

  // paid ist endstation, dann kommt kein button mehr
  if (NEXT_STATUS[order.status]) {
    var transition = NEXT_STATUS[order.status];
    var actionBtn = document.createElement('button');
    actionBtn.className = 'btn-primary';
    actionBtn.textContent = transition.label;
    actionBtn.setAttribute('data-order-id', order.id);
    actionBtn.setAttribute('data-next-status', transition.next);
    actionBtn.addEventListener('click', function() {
      updateOrderStatus(order.id, transition.next);
    });
    btnGroup.appendChild(actionBtn);
  }

  var detailBtn = document.createElement('button');
  detailBtn.className = 'btn-secondary';
  detailBtn.textContent = 'View Details';
  detailBtn.setAttribute('data-order-id', order.id);
  detailBtn.addEventListener('click', function() {
    openModal(order);
  });
  btnGroup.appendChild(detailBtn);

  // kellner ruft den tisch an, andere richtung als sonst
  var callTableBtn = document.createElement('button');
  callTableBtn.className = 'btn-call-table btn-secondary';
  callTableBtn.textContent = 'Call Table';
  callTableBtn.setAttribute('data-order-id', order.id);
  btnGroup.appendChild(callTableBtn);

  card.appendChild(btnGroup);
  return card;
}

function updateOrderStatus(orderId, newStatus) {
  // server broadcastet danach order_update, das lädt die queue neu
  window.sendWsMessage({ type: 'update_order', order_id: orderId, status: newStatus });
}

function openModal(order) {
  pendingModalOrder = order;
  currentModalOrderId = order.id;
  var content = document.getElementById('modal-content');
  content.innerHTML = '';
  document.getElementById('order-modal-overlay').classList.remove('hidden');
  // modal leer aufmachen, die details kommen per order_data nach
  // (queue-daten haben keine items/notes)
  window.sendWsMessage({ type: 'get_order', order_id: order.id });
}

function renderModalContent(content, order) {
  content.innerHTML = '';

  var heading = document.createElement('h3');
  heading.textContent = 'Order #' + order.id + ' — Table ' + order.table_number;
  content.appendChild(heading);

  var badge = document.createElement('span');
  badge.className = 'badge badge-' + order.status;
  badge.textContent = window.statusLabel(order.status);
  content.appendChild(badge);

  var itemsHeading = document.createElement('p');
  itemsHeading.className = 'modal-section-label';
  itemsHeading.textContent = 'Items';
  content.appendChild(itemsHeading);

  var ul = document.createElement('ul');
  ul.className = 'modal-items-list';
  if (Array.isArray(order.items) && order.items.length > 0) {
    order.items.forEach(function(item) {
      var menuEntry = menuMap[item.menu_item_id];
      var li = document.createElement('li');
      li.className = 'modal-item-row';

      var nameEl = document.createElement('span');
      nameEl.className = 'modal-item-name';
      // nicht im menumap (z.b. menü geändert)? dann rohe id zeigen
      nameEl.textContent = menuEntry ? window.escapeHtml(menuEntry.name) : 'Item #' + item.menu_item_id;

      var rightEl = document.createElement('span');
      rightEl.className = 'modal-item-right';
      var qtyEl = document.createElement('span');
      qtyEl.className = 'modal-item-qty';
      qtyEl.textContent = '×' + item.quantity;
      rightEl.appendChild(qtyEl);
      if (menuEntry) {
        var priceEl = document.createElement('span');
        priceEl.className = 'modal-item-price';
        priceEl.textContent = window.formatPrice(menuEntry.price * item.quantity);
        rightEl.appendChild(priceEl);
      }

      li.appendChild(nameEl);
      li.appendChild(rightEl);
      ul.appendChild(li);
    });

    // summe nur wenn wir alle preise kennen, sonst wär sie zu niedrig
    var total = order.items.reduce(function(sum, item) {
      var entry = menuMap[item.menu_item_id];
      return sum + (entry ? entry.price * item.quantity : 0);
    }, 0);
    if (total > 0) {
      var totalRow = document.createElement('li');
      totalRow.className = 'modal-item-row modal-total-row';
      var totalLabel = document.createElement('span');
      totalLabel.textContent = 'Total';
      var totalAmt = document.createElement('span');
      totalAmt.className = 'modal-item-price';
      totalAmt.textContent = window.formatPrice(total);
      totalRow.appendChild(totalLabel);
      totalRow.appendChild(totalAmt);
      ul.appendChild(totalRow);
    }
  } else {
    var li = document.createElement('li');
    li.className = 'modal-item-row';
    li.textContent = 'No items';
    ul.appendChild(li);
  }
  content.appendChild(ul);

  if (order.notes && order.notes.trim() !== '') {
    var notesHeading = document.createElement('p');
    notesHeading.className = 'modal-section-label';
    notesHeading.textContent = 'Notes';
    content.appendChild(notesHeading);

    var notesEl = document.createElement('p');
    notesEl.className = 'modal-notes';
    notesEl.textContent = window.escapeHtml(order.notes); // auch escapen
    content.appendChild(notesEl);
  }

  if (order.timestamp) {
    var ts = new Date(
      typeof order.timestamp === 'number'
        ? order.timestamp * 1000 // sek -> millis
        : order.timestamp
    );
    var tsEl = document.createElement('p');
    tsEl.className = 'order-card-meta';
    tsEl.textContent = ts.toLocaleDateString() + ' ' + ts.toLocaleTimeString();
    content.appendChild(tsEl);
  }
}

function closeModal() {
  document.getElementById('order-modal-overlay').classList.add('hidden');
  // state resetten, sonst schreibt ein spätes order_data ins geschlossene modal
  currentModalOrderId = null;
  pendingModalOrder = null;
}

function setAvailability(busy) {
  // frei/busy an server, der verteilt eingehende calls danach
  window.sendWsMessage({ type: 'set_availability', busy: busy });
}

// ws-nachrichten
function onWsMessage(msg) {
  switch (msg.type) {

    case 'menu_data':
      if (Array.isArray(msg.items)) {
        msg.items.forEach(function(item) { menuMap[item.id] = item; });
      }
      break;

    case 'queue_data':
      renderQueue(msg.orders || []);
      break;

    case 'order_data':
      // nur reinschreiben wenn's noch das modal ist auf das wir warten
      if (pendingModalOrder && msg.id === pendingModalOrder.id) {
        var content = document.getElementById('modal-content');
        renderModalContent(content, msg);
        pendingModalOrder = null;
      }
      break;

    case 'order_updated':
      loadQueue(); // einfach ganze queue neu laden
      break;

    case 'order_error':
      window.showToast('Could not update order. Try again.');
      break;

    case 'availability_ok':
      break; // ui wurde schon lokal umgeschaltet

    case 'availability_error':
      window.showToast('Could not update availability.');
      break;

    case 'resolve_ok':
      break;

    case 'resolve_error':
      window.showToast('Could not dismiss call. Try again.');
      break;

    case 'new_order':
      loadQueue();
      window.showToast(msg.table_number
        ? 'New order received for table ' + msg.table_number
        : 'New order received.');
      break;

    case 'order_update':
      loadQueue();
      break;

    case 'call_ring':
      // tischnr jetzt merken, call_accept liefert sie später nicht mehr mit
      if (msg.table_number) { _activeCallTable = msg.table_number; }
      if (typeof window.rtcHandleSignal === 'function') window.rtcHandleSignal(msg);
      break;

    case 'call_accept':
      // erst panel + heading zeigen, dann an webrtc.js weiter, sonst starrt der kellner
      // kurz auf ein leeres panel während der offer verarbeitet wird
      (function() {
        var overlay = document.getElementById('call-overlay');
        if (overlay) overlay.classList.add('hidden');
        var wca = document.getElementById('waiter-call-active');
        if (wca) wca.classList.remove('hidden');
        var heading = wca ? wca.querySelector('.call-panel-heading') : null;
        if (heading) heading.textContent = 'In Call — Table ' + _activeCallTable;
        var whb = document.getElementById('waiter-hangup-btn');
        if (whb) whb.focus();
      })();
      if (typeof window.rtcHandleSignal === 'function') window.rtcHandleSignal(msg);
      break;

    case 'call_reject':
    case 'call_busy':
    case 'call_end':
    case 'webrtc_offer':
    case 'webrtc_answer':
    case 'webrtc_ice':
      // reines signaling, direkt an webrtc.js durchreichen
      if (typeof window.rtcHandleSignal === 'function') window.rtcHandleSignal(msg);
      break;

  }
}

// bei jedem (re)connect state neu vom server holen, sonst hängt nach nem
// verbindungsabriss altes zeug in der ui
function onWsReady() {
  loadMenu();
  loadQueue();
}

window.addEventListener('DOMContentLoaded', function() {
  // ?role=waiter -> server markiert uns als kellner (is_waiter=1)
  window.wsInit(onWsMessage, onWsReady, '?role=waiter');
  if (typeof window.rtcInit === 'function') window.rtcInit();

  // checked = verfügbar, sonst busy
  document.getElementById('availability-toggle')
    .addEventListener('change', function(e) {
      var busy = !e.target.checked;
      setAvailability(busy);
      document.getElementById('availability-label').textContent =
        busy ? 'Busy' : 'Available';
    });

  document.getElementById('modal-close')
    .addEventListener('click', closeModal);

  // klick auf den hintergrund schließt das modal auch
  document.getElementById('order-modal-overlay')
    .addEventListener('click', function(e) {
      if (e.target === document.getElementById('order-modal-overlay')) {
        closeModal();
      }
    });

  // reihenfolge: erst server per ws bescheid, dann lokal rtc abbauen
  // kein table_number mitschicken, der kellner hat keine, server kennt den peer
  var waiterHangupBtn = document.getElementById('waiter-hangup-btn');
  if (waiterHangupBtn) {
    waiterHangupBtn.addEventListener('click', function() {
      window.sendWsMessage({ type: 'call_end' });
      window.rtcHangup();
      _activeCallTable = null; // call vorbei
    });
  }

  // delegation auf #order-queue: die karten werden dauernd neu gebaut,
  // einzelne listener müsste man bei jedem renderQueue neu setzen
  var orderContainer = document.getElementById('order-queue');
  if (orderContainer) {
    orderContainer.addEventListener('click', function(e) {
      if (e.target.classList.contains('btn-call-table')) {
        // tischnr steckt an der karte (data-table), nicht am button
        var card = e.target.closest('[data-table]');
        var tableNum = card ? card.dataset.table : null;
        if (!tableNum) return;

        window.sendWsMessage({ type: 'call_ring', table_number: Number(tableNum) });

        var wca = document.getElementById('waiter-call-active');
        if (wca) wca.classList.remove('hidden');
        var heading = wca ? wca.querySelector('.call-panel-heading') : null;
        if (heading) heading.textContent = 'Calling — Table ' + tableNum;

        _activeCallTable = Number(tableNum); // fürs heading merken
      }
    });
  }
});
})();
