#!/usr/bin/env python3
"""Generate clean architectural and simulated protocol conversation diagrams for Lab 2."""

import os
from pathlib import Path

os.environ.setdefault("MPLCONFIGDIR", "/tmp/mpl_lab2_figs")

import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
from matplotlib.patches import FancyBboxPatch, FancyArrowPatch

OUT_DIR = Path(__file__).resolve().parent / "figures"

BG = "#f6f8fb"
CARD = "#ffffff"
NAVY = "#152b46"
SLATE = "#475569"
BORDER = "#cbd5e1"
BLUE = "#2864b7"
BLUE_BG = "#eff6ff"
TEAL = "#168477"
TEAL_BG = "#ecfdf5"
AMBER = "#b45309"
AMBER_BG = "#fffbeb"
RED = "#ca5548"
RED_BG = "#fef2f2"
GRAY_BG = "#f1f5f9"

plt.rcParams.update({
    "font.family": "DejaVu Sans",
    "savefig.dpi": 240,
})


def add_box(ax, x, y, w, h, title, body="", fc=CARD, ec=BORDER, title_color=NAVY,
            lw=1.4, title_size=10.5, body_size=8.7, align="center"):
    patch = FancyBboxPatch(
        (x, y), w, h,
        boxstyle="round,pad=0.012,rounding_size=0.018",
        facecolor=fc, edgecolor=ec, linewidth=lw, zorder=2
    )
    ax.add_patch(patch)
    if body:
        ty = y + h * 0.72
        by = y + h * 0.32
        ax.text(x + w / 2 if align == "center" else x + 0.02, ty, title,
                ha=align, va="center", fontsize=title_size, fontweight="bold",
                color=title_color, zorder=3)
        ax.text(x + w / 2 if align == "center" else x + 0.02, by, body,
                ha=align, va="center", fontsize=body_size, color=SLATE,
                linespacing=1.32, zorder=3)
    else:
        ax.text(x + w / 2, y + h / 2, title, ha="center", va="center",
                fontsize=title_size, fontweight="bold", color=title_color, zorder=3)
    return patch


def add_arrow(ax, x1, y1, x2, y2, color=NAVY, lw=1.6, ls="-", rad=0.0):
    arr = FancyArrowPatch(
        (x1, y1), (x2, y2),
        arrowstyle="-|>,head_length=6.5,head_width=3.8",
        color=color, linewidth=lw, linestyle=ls,
        connectionstyle=f"arc3,rad={rad}", zorder=4
    )
    ax.add_patch(arr)


def seq_msg(ax, x1, x2, y, label, payload="", color=BLUE, ls="-", badge=None, label_size=8.5, payload_size=7.5):
    add_arrow(ax, x1, y, x2, y, color=color, lw=1.7, ls=ls)
    mid = (x1 + x2) / 2
    if label:
        ax.text(
            mid, y + 0.011, label, ha="center", va="bottom",
            fontsize=label_size, fontweight="bold", color=color,
            bbox=dict(boxstyle="round,pad=0.14", fc=BG, ec="none"), zorder=5
        )
    if payload:
        ax.text(
            mid, y - 0.012, payload, ha="center", va="top",
            fontsize=payload_size, family="DejaVu Sans Mono", color=NAVY,
            bbox=dict(boxstyle="round,pad=0.18", fc=CARD, ec=BORDER, lw=0.8), zorder=5
        )
    if badge:
        bx = x1 - 0.015 if x1 < x2 else x1 + 0.015
        ha = "right" if x1 < x2 else "left"
        ax.text(bx, y, badge, ha=ha, va="center", fontsize=7.6,
                fontweight="bold", color=SLATE, zorder=5)


def make_fig_architecture():
    fig, ax = plt.subplots(figsize=(14, 7.6), facecolor=BG)
    ax.set_xlim(0, 1)
    ax.set_ylim(0, 1)
    ax.axis("off")

    ax.text(0.025, 0.955, "Coffee Shop System Architecture & Server Concurrency Model",
            fontsize=15.5, fontweight="bold", color=NAVY)
    ax.text(0.025, 0.918,
            "Direct TCP sockets (\\n-delimited UTF-8 JSON) | Per-client reader threads + ThreadPool workers + mutex-protected shared stock",
            fontsize=9.5, color=SLATE)

    srv_bg = FancyBboxPatch(
        (0.28, 0.06), 0.695, 0.82,
        boxstyle="round,pad=0.012,rounding_size=0.02",
        facecolor="#eef2f7", edgecolor="#94a3b8", linewidth=1.4, linestyle="--", zorder=1
    )
    ax.add_patch(srv_bg)
    ax.text(0.628, 0.855, "C++23 TCP Server  (lab2/server/src/server.cpp)",
            ha="center", va="center", fontsize=11.5, fontweight="bold", color=NAVY)

    add_box(ax, 0.025, 0.63, 0.21, 0.21,
            "Java Client(s)  (role=client)",
            "Main.java + ServerListener\n- stdin command loop\n- Daemon reader thread\n- HEARTBEAT every 1.5 s",
            fc=BLUE_BG, ec=BLUE, title_color=BLUE)

    add_box(ax, 0.025, 0.34, 0.21, 0.21,
            "Java Provider  (role=provider)",
            "Main.java + ServerListener\n- Sends PROVIDER_IDENTIFY\n- Sends DELIVERY updates\n- Receives async LOW_STOCK",
            fc=TEAL_BG, ec=TEAL, title_color=TEAL)

    add_box(ax, 0.025, 0.09, 0.21, 0.17,
            "TCP Wire Protocol",
            "UTF-8 JSON lines ending in \\n\nNo HTTP / RPC wrappers\n11 MessageTypes, 6 ErrorCodes",
            fc=CARD, ec=BORDER, title_color=NAVY)

    add_box(ax, 0.305, 0.68, 0.19, 0.13,
            "Accept Loop (main)",
            "socket -> bind -> listen\npoll(500 ms) + accept()\nSpawns 1 jthread / client",
            fc=CARD, ec=NAVY, title_color=NAVY)

    add_box(ax, 0.305, 0.38, 0.19, 0.23,
            "ClientSession Thread(s)",
            "poll() with 3 s HEARTBEAT\nline_buffer (\\n split, <=8 KiB)\nsend_mutex + send_all()\nis_provider flag\nactive_orders + stop_source",
            fc=BLUE_BG, ec=BLUE, title_color=BLUE)

    add_box(ax, 0.305, 0.11, 0.19, 0.19,
            "Disconnect / Timeout",
            "On recv<=0 or >3 s no ping:\nstop_source.request_stop()\nAtomic finalized restore\nClear active_provider_",
            fc=RED_BG, ec=RED, title_color=RED)

    add_box(ax, 0.54, 0.65, 0.20, 0.16,
            "Shared Shop State",
            "stock_ (10..50 initially)\nnext_order_id_\nGuarded by stock_mutex_\nactive_provider_ (provider_mutex_)",
            fc=AMBER_BG, ec=AMBER, title_color=AMBER)

    add_box(ax, 0.54, 0.39, 0.20, 0.18,
            "ThreadSafeQueue",
            "Mutex + condition_variable\nFIFO std::packaged_task<void()>\nEach task captures orderId,\n(ip, port), items, stop_token",
            fc=CARD, ec=NAVY, title_color=NAVY)

    add_box(ax, 0.54, 0.11, 0.20, 0.19,
            "Protocol Validation",
            "protocol.h (nlohmann::json)\nStrict keys, types & ranges\nThrows ProtocolException\n-> ERROR + code + message",
            fc=CARD, ec=BORDER, title_color=NAVY)

    add_box(ax, 0.775, 0.39, 0.18, 0.42,
            "ThreadPool Workers",
            "N = hardware_concurrency()\nstd::jthread workers\n\nPrepares 1 order at a time:\n4s coffee, 3s tea, 2s crois.\n\ncv.wait_for(stop_token, eta)\n- Normal: send ORDER_READY\n- Stopped: exit immediately",
            fc=TEAL_BG, ec=TEAL, title_color=TEAL)

    add_box(ax, 0.775, 0.11, 0.18, 0.19,
            "Thread-Safe Logger",
            "log_mutex_ guards std::cout\nLogs [ip:port] on connect,\naccept/refuse/ready/cancel,\ndelivery, low-stock & errors",
            fc=GRAY_BG, ec=BORDER, title_color=NAVY)

    add_arrow(ax, 0.235, 0.74, 0.305, 0.74, color=BLUE)
    add_arrow(ax, 0.235, 0.45, 0.305, 0.49, color=TEAL)
    add_arrow(ax, 0.40, 0.68, 0.40, 0.61, color=NAVY)
    add_arrow(ax, 0.40, 0.38, 0.40, 0.30, color=RED)
    add_arrow(ax, 0.495, 0.56, 0.54, 0.72, color=AMBER)
    add_arrow(ax, 0.495, 0.48, 0.54, 0.48, color=NAVY)
    add_arrow(ax, 0.495, 0.41, 0.54, 0.21, color=SLATE)
    # Route stock restore along the gap between columns rather than across ClientSession text
    add_arrow(ax, 0.495, 0.24, 0.54, 0.68, color=RED, ls="--", rad=-0.08)
    add_arrow(ax, 0.74, 0.48, 0.775, 0.56, color=TEAL)
    add_arrow(ax, 0.775, 0.41, 0.495, 0.41, color=TEAL, ls="--", rad=-0.22)
    add_arrow(ax, 0.74, 0.20, 0.775, 0.20, color=SLATE)

    ax.text(0.025, 0.025, "Note: Dashed lines indicate asynchronous worker replies (ORDER_READY) and cancellation stock restoration.",
            fontsize=8.5, color=SLATE)
    fig.savefig(OUT_DIR / "fig_architecture.png", bbox_inches="tight", pad_inches=0.18)
    plt.close(fig)


def make_fig_client_order_conv():
    fig, ax = plt.subplots(figsize=(13.8, 9.6), facecolor=BG)
    ax.set_xlim(0, 1)
    ax.set_ylim(0, 1)
    ax.axis("off")

    ax.text(0.025, 0.968, "Simulated Conversation 1: Client Menu, Concurrent Ordering & Asynchronous Completion",
            fontsize=14.5, fontweight="bold", color=NAVY)
    ax.text(0.025, 0.938,
            "Illustrative timeline (coffee=4s, tea=3s, croissant=2s). Every JSON message is UTF-8 and terminated with \\n.",
            fontsize=9.3, color=SLATE)

    x_cli, x_srv, x_wrk = 0.16, 0.54, 0.87
    add_box(ax, x_cli - 0.11, 0.855, 0.22, 0.058, "Java Client\n127.0.0.1:51420", fc=BLUE_BG, ec=BLUE, title_color=BLUE, title_size=9.8)
    add_box(ax, x_srv - 0.11, 0.855, 0.22, 0.058, "C++ Server\nClientSession + Stock", fc=CARD, ec=NAVY, title_color=NAVY, title_size=9.8)
    add_box(ax, x_wrk - 0.10, 0.855, 0.20, 0.058, "ThreadPool\nWorker #1", fc=TEAL_BG, ec=TEAL, title_color=TEAL, title_size=9.8)

    for x, col in ((x_cli, BLUE), (x_srv, NAVY), (x_wrk, TEAL)):
        ax.plot([x, x], [0.035, 0.855], color=col, lw=1.4, ls=":", zorder=1)

    seq_msg(ax, x_cli, x_srv, 0.805, "1. Menu Request", '{"type":"MENU_REQUEST"}', color=BLUE, badge="t=0.0s")
    seq_msg(ax, x_srv, x_cli, 0.730, "2. Menu Snapshot", '{"type":"MENU_RESPONSE","items":{"coffee":24,"tea":31,"croissant":18}}', color=NAVY)
    seq_msg(ax, x_cli, x_srv, 0.655, "3. Background Keepalive", '{"type":"HEARTBEAT"}', color=SLATE, ls="--", badge="t=1.5s")
    seq_msg(ax, x_cli, x_srv, 0.580, "4. Place Order (1*4 + 1*3 + 2*2 = 11s)", '{"type":"ORDER_REQUEST","items":{"coffee":1,"tea":1,"croissant":2}}', color=BLUE, badge="t=2.0s")

    prep_box = FancyBboxPatch(
        (x_wrk - 0.06, 0.115), 0.12, 0.405,
        boxstyle="round,pad=0.008,rounding_size=0.012",
        facecolor=TEAL_BG, edgecolor=TEAL, linewidth=1.3, zorder=2
    )
    ax.add_patch(prep_box)
    ax.text(x_wrk, 0.315, "Preparing\nOrder #1\n(11 s wait)", ha="center", va="center",
            fontsize=8.6, fontweight="bold", color=TEAL, zorder=3)

    seq_msg(ax, x_srv, x_wrk, 0.505, "Enqueue Task", "packaged_task(#1, 11s, stop_token)", color=TEAL)
    seq_msg(ax, x_srv, x_cli, 0.435, "5. Immediate Acceptance (stock deducted)", '{"type":"ORDER_ACCEPTED","orderId":1,"preparationTime":11}', color=TEAL)
    seq_msg(ax, x_cli, x_srv, 0.360, "6. Non-blocking Menu Query while waiting", '{"type":"MENU_REQUEST"}', color=BLUE, badge="t=4.0s")
    seq_msg(ax, x_srv, x_cli, 0.285, "7. Updated Stock Visible Immediately", '{"type":"MENU_RESPONSE","items":{"coffee":23,"tea":30,"croissant":16}}', color=NAVY)
    seq_msg(ax, x_cli, x_srv, 0.210, "8. Over-Stock Order", '{"type":"ORDER_REQUEST","items":{"coffee":99}}', color=RED, badge="t=5.0s")
    seq_msg(ax, x_srv, x_cli, 0.135, "9. Refusal (stock unchanged)", '{"type":"ORDER_REFUSED","reason":"Insufficient coffee (requested 99, available 23)"}', color=RED)
    seq_msg(ax, x_wrk, x_cli, 0.065, "10. Async Completion (guarded by session->send_mutex)", '{"type":"ORDER_READY","orderId":1}', color=TEAL, ls="-", badge="t=13.0s")

    fig.savefig(OUT_DIR / "fig_client_order_conv.png", bbox_inches="tight", pad_inches=0.18)
    plt.close(fig)


def make_fig_provider_lowstock_conv():
    fig, ax = plt.subplots(figsize=(13.8, 9.6), facecolor=BG)
    ax.set_xlim(0, 1)
    ax.set_ylim(0, 1)
    ax.axis("off")

    ax.text(0.025, 0.968, "Simulated Conversation 2: Provider Identification, Low-Stock Alerts & Delivery",
            fontsize=14.5, fontweight="bold", color=NAVY)
    ax.text(0.025, 0.938,
            "Server rejects client deliveries with UNAUTHORIZED_OPERATION, alerts provider when stock < 5, and updates stock atomically.",
            fontsize=9.3, color=SLATE)

    x_cli, x_srv, x_prov = 0.14, 0.50, 0.86
    add_box(ax, x_cli - 0.10, 0.855, 0.20, 0.058, "Java Client\n(is_provider=false)", fc=BLUE_BG, ec=BLUE, title_color=BLUE, title_size=9.8)
    add_box(ax, x_srv - 0.11, 0.855, 0.22, 0.058, "C++ Server\n(tea=31, coffee=23)", fc=CARD, ec=NAVY, title_color=NAVY, title_size=9.8)
    add_box(ax, x_prov - 0.10, 0.855, 0.20, 0.058, "Java Provider\n(is_provider=true)", fc=TEAL_BG, ec=TEAL, title_color=TEAL, title_size=9.8)

    for x, col in ((x_cli, BLUE), (x_srv, NAVY), (x_prov, TEAL)):
        ax.plot([x, x], [0.035, 0.855], color=col, lw=1.4, ls=":", zorder=1)

    seq_msg(ax, x_cli, x_srv, 0.805, "1. Unauthorized Delivery Attempt", '{"type":"DELIVERY","items":{"coffee":5}}', color=RED, badge="t=0.0s")
    seq_msg(ax, x_srv, x_cli, 0.730, "2. Rejected (not provider)", '{"type":"ERROR","code":"UNAUTHORIZED_OPERATION","message":"Only an identified provider..."}', color=RED)

    seq_msg(ax, x_cli, x_srv, 0.650, "3. Order drops tea 31 -> 3 (< 5) before provider connects", '{"type":"ORDER_REQUEST","items":{"tea":28}}', color=BLUE, badge="t=1.0s")
    seq_msg(ax, x_srv, x_cli, 0.575, "4. Accepted (tea now 3)", '{"type":"ORDER_ACCEPTED","orderId":2,"preparationTime":84}', color=NAVY)

    seq_msg(ax, x_prov, x_srv, 0.500, "5. Provider Connects & Identifies", '{"type":"PROVIDER_IDENTIFY"}', color=TEAL, badge="t=2.5s")
    seq_msg(ax, x_srv, x_prov, 0.425, "6. Immediate Low-Stock Catch-Up", '{"type":"LOW_STOCK","product":"tea","remaining":3}', color=AMBER)

    seq_msg(ax, x_cli, x_srv, 0.350, "7. Order drops coffee 23 -> 4 (< 5)", '{"type":"ORDER_REQUEST","items":{"coffee":19}}', color=BLUE, badge="t=4.0s")
    seq_msg(ax, x_srv, x_cli, 0.275, "8. Accepted", '{"type":"ORDER_ACCEPTED","orderId":3,"preparationTime":76}', color=NAVY)
    seq_msg(ax, x_srv, x_prov, 0.275, "9. Live Push Alert", '{"type":"LOW_STOCK","product":"coffee","remaining":4}', color=AMBER)

    seq_msg(ax, x_prov, x_srv, 0.200, "10. Stock Replenishment (no reply on success)", '{"type":"DELIVERY","items":{"coffee":20,"tea":15}}', color=TEAL, badge="t=5.0s")
    seq_msg(ax, x_cli, x_srv, 0.130, "11. Menu Query After Delivery", '{"type":"MENU_REQUEST"}', color=BLUE, badge="t=6.0s")
    seq_msg(ax, x_srv, x_cli, 0.060, "12. New Stock Reflected", '{"type":"MENU_RESPONSE","items":{"coffee":24,"tea":18,"croissant":16}}', color=NAVY)

    fig.savefig(OUT_DIR / "fig_provider_lowstock_conv.png", bbox_inches="tight", pad_inches=0.18)
    plt.close(fig)


def make_fig_failure_framing_conv():
    fig, (ax1, ax2) = plt.subplots(1, 2, figsize=(14.5, 7.8), facecolor=BG)
    fig.subplots_adjust(left=0.03, right=0.98, top=0.84, bottom=0.07, wspace=0.12)
    fig.suptitle("Simulated Conversation 3: Disconnect Cancellation, Heartbeat Timeout & TCP Stream Framing",
                 x=0.03, y=0.955, ha="left", fontsize=14, fontweight="bold", color=NAVY)
    fig.text(0.03, 0.905,
             "Left: Abrupt disconnect or 3 s heartbeat timeout stops worker tasks via stop_token and restores reserved stock once.   "
             "Right: Byte-stream framing & error isolation.",
             fontsize=9.2, color=SLATE)

    ax1.set_xlim(0, 1)
    ax1.set_ylim(0, 1)
    ax1.axis("off")
    add_box(ax1, 0.01, 0.01, 0.98, 0.98, "", fc=CARD, ec=BORDER, lw=1.2)
    ax1.text(0.50, 0.95, "Failure Recovery: Heartbeat Timeout / SIGKILL",
             ha="center", va="center", fontsize=11, fontweight="bold", color=NAVY)

    x_b, x_s, x_w = 0.17, 0.52, 0.85
    add_box(ax1, x_b - 0.12, 0.85, 0.24, 0.065, "Client B", fc=BLUE_BG, ec=BLUE, title_color=BLUE, title_size=9.2)
    add_box(ax1, x_s - 0.12, 0.85, 0.24, 0.065, "Server", fc=CARD, ec=NAVY, title_color=NAVY, title_size=9.2)
    add_box(ax1, x_w - 0.11, 0.85, 0.22, 0.065, "Worker / Queue", fc=TEAL_BG, ec=TEAL, title_color=TEAL, title_size=9.2)
    for x, col in ((x_b, BLUE), (x_s, NAVY), (x_w, TEAL)):
        ax1.plot([x, x], [0.06, 0.85], color=col, lw=1.2, ls=":", zorder=1)

    seq_msg(ax1, x_b, x_s, 0.785, "Order #4 (coffee:2, 8s)", '{"type":"ORDER_REQUEST","items":{"coffee":2}}', color=BLUE)
    seq_msg(ax1, x_s, x_b, 0.695, "Accepted #4 (stock -2)", '{"type":"ORDER_ACCEPTED","orderId":4,...}', color=NAVY)
    seq_msg(ax1, x_s, x_w, 0.610, "Submit #4 + #5", "packaged_task(stop_token)", color=TEAL)

    add_box(ax1, 0.05, 0.445, 0.42, 0.10,
            "Client Disappears (t=3.0 s)",
            "Either SIGKILL (recv == 0 / ECONNRESET)\nor no HEARTBEAT within 3.0 s",
            fc=RED_BG, ec=RED, title_color=RED, title_size=8.8, body_size=7.7)

    seq_msg(ax1, x_s, x_w, 0.365, "stop_source.request_stop()", "Wakes cv.wait_for immediately", color=RED)
    add_box(ax1, 0.35, 0.20, 0.60, 0.105,
            "Atomic Stock Restoration (finalized.exchange(true))",
            "Restores coffee=+2 (and queued #5) exactly once.\nWorker exits without sending ORDER_READY.",
            fc=AMBER_BG, ec=AMBER, title_color=AMBER, title_size=8.6, body_size=7.7)
    add_box(ax1, 0.06, 0.06, 0.88, 0.095,
            "Shop Remains Healthy",
            "Other clients continue querying menu & ordering; provider disconnect also leaves server active.",
            fc=TEAL_BG, ec=TEAL, title_color=TEAL, title_size=8.6, body_size=7.7)

    ax2.set_xlim(0, 1)
    ax2.set_ylim(0, 1)
    ax2.axis("off")
    add_box(ax2, 0.01, 0.01, 0.98, 0.98, "", fc=CARD, ec=BORDER, lw=1.2)
    ax2.text(0.50, 0.95, "TCP Byte-Stream Framing & Input Isolation",
             ha="center", va="center", fontsize=11, fontweight="bold", color=NAVY)

    add_box(ax2, 0.06, 0.755, 0.88, 0.14,
            "1. Split & Coalesced TCP Packets -> line_buffer",
            'recv() #1: {"type":"MENU_   (no \\n -> buffered, nothing parsed yet)\n'
            'recv() #2: REQUEST"}\\n{"type":"HEARTBEAT"}\\n{"type":"MENU_REQUEST"}\\n\n'
            'while (nl = line_buffer.find("\\n")) extracts 3 complete lines sequentially.',
            fc=BLUE_BG, ec=BLUE, title_color=BLUE, title_size=9.1, body_size=7.8)

    add_box(ax2, 0.06, 0.565, 0.88, 0.145,
            "2. Protocol & Schema Validation -> ERROR (Session Kept Alive)",
            '"{broken"        -> ERROR INVALID_MESSAGE\n'
            '{"items":{"tea":-1}} -> ERROR INVALID_QUANTITY\n'
            '{"items":{"latte":1}} -> ERROR UNKNOWN_PRODUCT',
            fc=RED_BG, ec=RED, title_color=RED, title_size=9.1, body_size=7.8)

    add_box(ax2, 0.06, 0.375, 0.88, 0.145,
            "3. Oversized Line Guard (MAX_LINE_BYTES = 8192)",
            "If line > 8192 bytes without \\n: emit one ERROR INVALID_MESSAGE,\n"
            "clear buffer, discard bytes until next \\n, then resume normal JSON lines.",
            fc=AMBER_BG, ec=AMBER, title_color=AMBER, title_size=9.1, body_size=7.8)

    add_box(ax2, 0.06, 0.10, 0.88, 0.22,
            "4. Full-Duplex Client & Thread-Safe Server Sends",
            "Java Main: main thread blocks on stdin while daemon ServerListener\n"
            "blocks on BufferedReader.readLine(), printing async ORDER_READY / LOW_STOCK\n"
            "immediately while user types. Server wraps every send() in session->send_mutex.",
            fc=TEAL_BG, ec=TEAL, title_color=TEAL, title_size=9.1, body_size=7.8)

    fig.savefig(OUT_DIR / "fig_failure_framing_conv.png", bbox_inches="tight", pad_inches=0.18)
    plt.close(fig)


def main():
    OUT_DIR.mkdir(parents=True, exist_ok=True)
    make_fig_architecture()
    make_fig_client_order_conv()
    make_fig_provider_lowstock_conv()
    make_fig_failure_framing_conv()
    print(f"Generated figures in {OUT_DIR}")


if __name__ == "__main__":
    main()
