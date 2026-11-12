
import json
import networkx as nx

from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import urlparse, parse_qs

from os_ken.base import app_manager
from os_ken.controller import ofp_event
from os_ken.controller.handler import (
    CONFIG_DISPATCHER,
    MAIN_DISPATCHER,
    set_ev_cls,
)
from os_ken.ofproto import ofproto_v1_3
from os_ken.lib.packet import packet, ethernet, ether_types
from os_ken.lib import hub


# Switch-to-switch links: (switch A, switch B, port A, port B)
LINKS = [
    (1, 2, 2, 1),
    (1, 3, 3, 1),
    (2, 4, 2, 1),
    (3, 4, 2, 2),
]

SWITCH_NAMES = {
    1: "s1",
    2: "s2",
    3: "s3",
    4: "s4",
}

# Directly attached hosts:
# h1 = s1 port 1
# manager = s1 port 4
# h2 = s4 port 3
HOST_PORTS = {
    1: {1, 4},
    4: {3},
}


# ============================================================
# HTTP ROUTE API
# ============================================================

class RouteAPIHandler(BaseHTTPRequestHandler):
    controller = None

    def do_GET(self):
        parsed = urlparse(self.path)

        if parsed.path != "/route":
            self.send_error(404, "Endpoint not found")
            return

        worker = parse_qs(parsed.query).get("worker", [None])[0]

        targets = {
            "worker1": 1,
            "worker2": 4,
        }

        if worker not in targets:
            self.send_error(404, "Unknown worker")
            return

        controller = RouteAPIHandler.controller

        if controller is None:
            self.send_error(503, "Controller not ready")
            return

        if not controller.worker_status.get(worker, True):
            response = {
                "worker": worker,
                "path": [],
                "status": "unavailable",
            }
            body = json.dumps(response).encode("utf-8")
            self.send_response(503)
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)
            return

        try:
            path = controller.get_switch_path(1, targets[worker])

            if not path:
                raise nx.NetworkXNoPath("No route available")

            response = {
                "worker": worker,
                "path": [SWITCH_NAMES[s] for s in path],
                "status": "available",
            }

            status = 200

        except (nx.NetworkXNoPath, nx.NodeNotFound):
            response = {
                "worker": worker,
                "path": [],
                "status": "unavailable",
            }

            status = 503

        body = json.dumps(response).encode("utf-8")

        self.send_response(status)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def do_POST(self):
        if urlparse(self.path).path != "/worker-status":
            self.send_error(404, "Endpoint not found")
            return

        controller = RouteAPIHandler.controller
        if controller is None:
            self.send_error(503, "Controller not ready")
            return

        try:
            length = int(self.headers.get("Content-Length", "0"))
            if length <= 0 or length > 4096:
                raise ValueError("Invalid request length")

            data = json.loads(self.rfile.read(length).decode("utf-8"))
            worker = data.get("worker")
            status = data.get("status")

            if worker not in ("worker1", "worker2"):
                raise ValueError("Unknown worker")
            if status not in ("available", "unavailable"):
                raise ValueError("Invalid status")

            controller.set_worker_status(worker, status == "available")

            body = json.dumps({
                "worker": worker,
                "status": status,
                "updated": True,
            }).encode("utf-8")

            self.send_response(200)
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)

        except (ValueError, TypeError, json.JSONDecodeError) as exc:
            self.send_error(400, str(exc))

    def log_message(self, fmt, *args):
        print("Route API:", fmt % args, flush=True)


def start_route_api():

    class ReusableThreadingHTTPServer(ThreadingHTTPServer):
        allow_reuse_address = True
        daemon_threads = True

    server = ReusableThreadingHTTPServer(
        ("0.0.0.0", 5000),
        RouteAPIHandler,
    )

    print("SDN Route API listening on port 5000", flush=True)

    server.serve_forever()


# ============================================================
# SDN CONTROLLER
# ============================================================

class JobSDNController(app_manager.OSKenApp):

    OFP_VERSIONS = [ofproto_v1_3.OFP_VERSION]

    def __init__(self, *args, **kwargs):

        super(JobSDNController, self).__init__(*args, **kwargs)

        self.datapaths = {}

        self.graph = nx.Graph()
        self.graph.add_nodes_from(SWITCH_NAMES.keys())

        self.mac_locations = {}
        self.link_ports = {}

        self.worker_status = {
            "worker1": True,
            "worker2": True,
        }
        self.worker_macs = {
            "worker1": None,
            "worker2": None,
        }

        # Initialize topology
        for a, b, port_a, port_b in LINKS:

            self.link_ports[(a, port_a)] = (b, port_b)
            self.link_ports[(b, port_b)] = (a, port_a)

            self.graph.add_edge(
                a,
                b,
                ports={
                    a: port_a,
                    b: port_b,
                },
            )

        # Register controller with HTTP API
        RouteAPIHandler.controller = self

        # Start HTTP API using OS-Ken's hub scheduler
        self.api_thread = hub.spawn(start_route_api)

        self.logger.info("Dynamic Job SDN Controller started")

    # ========================================================
    # APPLICATION-DRIVEN WORKER CONTROL
    # ========================================================

    def set_worker_status(self, worker, available):
        self.worker_status[worker] = available
        mac = self.worker_macs.get(worker)

        if mac is not None:
            for datapath in list(self.datapaths.values()):
                parser = datapath.ofproto_parser
                ofproto = datapath.ofproto
                match = parser.OFPMatch(eth_dst=mac)

                # Remove old forwarding rules for this worker.
                datapath.send_msg(parser.OFPFlowMod(
                    datapath=datapath,
                    command=ofproto.OFPFC_DELETE,
                    out_port=ofproto.OFPP_ANY,
                    out_group=ofproto.OFPG_ANY,
                    match=match,
                ))

                if not available:
                    # Block traffic destined for the failed worker.
                    self.add_flow(
                        datapath,
                        100,
                        match,
                        [],
                        idle_timeout=0,
                        hard_timeout=0,
                    )

        self.logger.warning(
            "APPLICATION SDN CONTROL: %s is now %s",
            worker,
            "AVAILABLE" if available else "UNAVAILABLE",
        )

    # ========================================================
    # ROUTING
    # ========================================================

    def get_switch_path(self, source, destination):

        if source not in self.graph or destination not in self.graph:
            return None

        try:
            return nx.shortest_path(
                self.graph,
                source,
                destination,
            )

        except (nx.NetworkXNoPath, nx.NodeNotFound):
            return None

    def get_output_port(self, current, next_switch):

        return self.graph[current][next_switch]["ports"][current]

    def next_hop_port(self, path, current):

        index = path.index(current)

        if index >= len(path) - 1:
            return None

        return self.get_output_port(
            current,
            path[index + 1],
        )

    # ========================================================
    # FLOW MANAGEMENT
    # ========================================================

    def add_flow(
        self,
        datapath,
        priority,
        match,
        actions,
        idle_timeout=0,
        hard_timeout=0,
    ):

        parser = datapath.ofproto_parser
        ofproto = datapath.ofproto

        instructions = [
            parser.OFPInstructionActions(
                ofproto.OFPIT_APPLY_ACTIONS,
                actions,
            )
        ]

        datapath.send_msg(
            parser.OFPFlowMod(
                datapath=datapath,
                priority=priority,
                match=match,
                instructions=instructions,
                idle_timeout=idle_timeout,
                hard_timeout=hard_timeout,
            )
        )

    def install_table_miss(self, datapath):

        parser = datapath.ofproto_parser
        ofproto = datapath.ofproto

        actions = [
            parser.OFPActionOutput(
                ofproto.OFPP_CONTROLLER,
                ofproto.OFPCML_NO_BUFFER,
            )
        ]

        # Permanent table-miss rule
        self.add_flow(
            datapath,
            0,
            parser.OFPMatch(),
            actions,
            idle_timeout=0,
            hard_timeout=0,
        )

    def clear_dynamic_flows(self):

        for datapath in list(self.datapaths.values()):

            parser = datapath.ofproto_parser
            ofproto = datapath.ofproto

            datapath.send_msg(
                parser.OFPFlowMod(
                    datapath=datapath,
                    command=ofproto.OFPFC_DELETE,
                    out_port=ofproto.OFPP_ANY,
                    out_group=ofproto.OFPG_ANY,
                    match=parser.OFPMatch(),
                    table_id=ofproto.OFPTT_ALL,
                )
            )

            self.install_table_miss(datapath)

        self.logger.info(
            "Cleared dynamic flows; restored table-miss rules"
        )

    # ========================================================
    # SWITCH CONNECTION
    # ========================================================

    @set_ev_cls(
        ofp_event.EventOFPSwitchFeatures,
        CONFIG_DISPATCHER,
    )
    def switch_features_handler(self, ev):

        datapath = ev.msg.datapath

        self.datapaths[datapath.id] = datapath

        self.install_table_miss(datapath)

        self.logger.info(
            "Switch %s connected; permanent table-miss installed",
            datapath.id,
        )

    # ========================================================
    # LINK FAILURE AND RECOVERY
    # ========================================================

    @set_ev_cls(
        ofp_event.EventOFPPortStatus,
        MAIN_DISPATCHER,
    )
    def port_status_handler(self, ev):

        msg = ev.msg
        datapath = msg.datapath

        key = (
            datapath.id,
            msg.desc.port_no,
        )

        if key not in self.link_ports:
            return

        neighbor, neighbor_port = self.link_ports[key]

        link_down = bool(
            msg.desc.state & datapath.ofproto.OFPPS_LINK_DOWN
        )

        edge = (datapath.id, neighbor)

        if link_down:

            if self.graph.has_edge(*edge):
                self.graph.remove_edge(*edge)

            self.logger.warning(
                "LINK DOWN: %s <-> %s",
                SWITCH_NAMES[datapath.id],
                SWITCH_NAMES[neighbor],
            )

        else:

            self.graph.add_edge(
                datapath.id,
                neighbor,
                ports={
                    datapath.id: msg.desc.port_no,
                    neighbor: neighbor_port,
                },
            )

            self.logger.info(
                "LINK UP: %s <-> %s",
                SWITCH_NAMES[datapath.id],
                SWITCH_NAMES[neighbor],
            )

        self.clear_dynamic_flows()

        self.logger.info(
            "Updated topology: %s",
            list(self.graph.edges()),
        )

    # ========================================================
    # LOOP-FREE BROADCAST
    # ========================================================

    def broadcast_ports(self, dpid, in_port):

        tree = nx.minimum_spanning_tree(self.graph)

        ports = set(HOST_PORTS.get(dpid, set()))

        for neighbor in tree.neighbors(dpid):

            ports.add(
                self.get_output_port(dpid, neighbor)
            )

        ports.discard(in_port)

        result = sorted(ports)

        self.logger.info(
            "BROADCAST: switch=%s in_port=%s out_ports=%s",
            dpid,
            in_port,
            result,
        )

        return result

    # ========================================================
    # UNICAST PATH INSTALLATION
    # ========================================================

    def install_path_flows(
        self,
        path,
        src,
        dst,
        source_location,
        destination_location,
    ):

        for index, switch_id in enumerate(path):

            datapath = self.datapaths.get(switch_id)

            if datapath is None:
                continue

            if index == 0:

                in_port = source_location[1]

            else:

                previous = path[index - 1]

                in_port = self.graph[previous][switch_id]["ports"][switch_id]

            if index == len(path) - 1:

                out_port = destination_location[1]

            else:

                next_switch = path[index + 1]

                out_port = self.graph[switch_id][next_switch]["ports"][switch_id]

            parser = datapath.ofproto_parser

            match = parser.OFPMatch(
                in_port=in_port,
                eth_src=src,
                eth_dst=dst,
            )

            self.add_flow(
                datapath,
                20,
                match,
                [
                    parser.OFPActionOutput(out_port)
                ],
                idle_timeout=30,
                hard_timeout=120,
            )

    # ========================================================
    # PACKET-IN HANDLER
    # ========================================================

    @set_ev_cls(
        ofp_event.EventOFPPacketIn,
        MAIN_DISPATCHER,
    )
    def packet_in_handler(self, ev):

        msg = ev.msg

        datapath = msg.datapath
        parser = datapath.ofproto_parser
        ofproto = datapath.ofproto

        pkt = packet.Packet(msg.data)

        eth = pkt.get_protocol(ethernet.ethernet)

        if eth is None or eth.ethertype == ether_types.ETH_TYPE_LLDP:
            return

        dpid = datapath.id
        in_port = msg.match["in_port"]

        src = eth.src
        dst = eth.dst

        # Learn MAC addresses from directly attached hosts
        if in_port in HOST_PORTS.get(dpid, set()):

            self.mac_locations[src] = (
                dpid,
                in_port,
            )

            worker = None
            if dpid == 1 and in_port == 1:
                worker = "worker1"
            elif dpid == 4 and in_port == 3:
                worker = "worker2"

            if worker is not None:
                self.worker_macs[worker] = src

                # Allow outbound traffic so an unavailable worker can
                # reconnect and register with the manager. The existing
                # priority-100 eth_dst drop rule blocks inbound traffic.

        # Broadcast and unknown destination handling
        if (
            dst == "ff:ff:ff:ff:ff:ff"
            or dst not in self.mac_locations
        ):

            actions = [
                parser.OFPActionOutput(port)
                for port in self.broadcast_ports(dpid, in_port)
            ]

        else:

            source_location = self.mac_locations.get(src)
            destination_location = self.mac_locations[dst]

            if source_location is None:

                self.logger.warning(
                    "Unknown source MAC %s on switch %s",
                    src,
                    dpid,
                )

                return

            path = self.get_switch_path(
                source_location[0],
                destination_location[0],
            )

            if path is None or dpid not in path:

                self.logger.warning(
                    "No usable route for destination MAC %s",
                    dst,
                )

                return

            self.install_path_flows(
                path,
                src,
                dst,
                source_location,
                destination_location,
            )

            out_port = self.next_hop_port(path, dpid)

            if out_port is None:
                out_port = destination_location[1]

            actions = [
                parser.OFPActionOutput(out_port)
            ]

            self.logger.info(
                "Installed path-wide flows: %s",
                [
                    SWITCH_NAMES[s]
                    for s in path
                ],
            )

        data = (
            msg.data
            if msg.buffer_id == ofproto.OFP_NO_BUFFER
            else None
        )

        datapath.send_msg(
            parser.OFPPacketOut(
                datapath=datapath,
                buffer_id=msg.buffer_id,
                in_port=in_port,
                actions=actions,
                data=data,
            )
        )
