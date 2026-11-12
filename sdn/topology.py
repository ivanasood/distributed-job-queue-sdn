from mininet.net import Mininet
from mininet.node import RemoteController, OVSSwitch
from mininet.cli import CLI
from mininet.log import setLogLevel, info


def create_topology():
    net = Mininet(
        controller=RemoteController,
        switch=OVSSwitch,
        autoSetMacs=True,
        build=False
    )

    info("*** Adding remote controller\n")
    net.addController(
        "c0",
        controller=RemoteController,
        ip="127.0.0.1",
        port=6633
    )

    info("*** Adding switches\n")
    s1 = net.addSwitch("s1", protocols="OpenFlow13")
    s2 = net.addSwitch("s2", protocols="OpenFlow13")
    s3 = net.addSwitch("s3", protocols="OpenFlow13")
    s4 = net.addSwitch("s4", protocols="OpenFlow13")

    info("*** Adding hosts\n")
    h1 = net.addHost("h1", ip="10.0.0.1/24")
    h2 = net.addHost("h2", ip="10.0.0.2/24")
    manager = net.addHost(
        "manager",
        ip="10.0.0.254/24",
        inNamespace=False
    )

    info("*** Creating links\n")
    net.addLink(h1, s1)
    net.addLink(s1, s2)
    net.addLink(s2, s4)
    net.addLink(s1, s3)
    net.addLink(s3, s4)
    net.addLink(s4, h2)
    net.addLink(manager, s1)

    info("*** Building network\n")
    net.build()

    info("*** Starting network\n")
    net.start()

    info("*** Starting Mininet CLI\n")
    CLI(net)

    info("*** Stopping network\n")
    net.stop()


if __name__ == "__main__":
    setLogLevel("info")
    create_topology()
