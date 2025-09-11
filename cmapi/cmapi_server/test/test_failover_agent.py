import logging
import socket
from unittest.mock import patch

from mcs_node_control.models.node_config import NodeConfig

from cmapi_server.failover_agent import FailoverAgent
from cmapi_server.managers.network import NetworkManager
from cmapi_server.node_manipulation import add_node, remove_node
from cmapi_server.test.unittest_global import BaseNodeManipTestCase, tmp_mcs_config_filename

logging.basicConfig(level='DEBUG')


class TestFailoverAgent(BaseNodeManipTestCase):

    def test_activateNodes(self):
        self.tmp_files = ('./activate0.xml', './activate1.xml')
        hostaddr = socket.gethostbyname(socket.gethostname())
        fa = FailoverAgent()
        # Bypass DNS validation for hostname-based addition in tests
        with patch.object(NetworkManager, 'validate_hostname_fwd_rev', return_value=None):
            fa.activateNodes(
                [self.NEW_NODE_NAME], tmp_mcs_config_filename, self.tmp_files[0],
                test_mode=True
            )
        add_node(
            hostaddr, self.tmp_files[0], self.tmp_files[1]
        )

        nc = NodeConfig()
        root = nc.get_current_config_root(self.tmp_files[1])
        pm_count = int(root.find('./PrimitiveServers/Count').text)
        self.assertEqual(pm_count, 2)
        node = root.find('./PMS1/IPAddr')
        self.assertEqual(node.text, self.NEW_NODE_NAME)
        node = root.find('./pm1_WriteEngineServer/IPAddr')
        self.assertEqual(node.text, self.NEW_NODE_NAME)
        node = root.find('./PMS2/IPAddr')
        self.assertEqual(node.text, hostaddr)
        node = root.find('./pm2_WriteEngineServer/IPAddr')
        self.assertEqual(node.text, hostaddr)
        remove_node(self.NEW_NODE_NAME, self.tmp_files[1], self.tmp_files[1])

    def test_deactivateNodes(self):
        self.tmp_files = (
            './deactivate0.xml','./deactivate1.xml', './deactivate2.xml'
        )
        fa = FailoverAgent()
        hostname = socket.gethostname()
        hostaddr = socket.gethostbyname(hostname)
        add_node(
            hostaddr, tmp_mcs_config_filename, self.tmp_files[0]
        )
        with patch.object(NetworkManager, 'validate_hostname_fwd_rev', return_value=None):
            fa.activateNodes(
                [self.NEW_NODE_NAME], self.tmp_files[0], self.tmp_files[1],
                test_mode=True
            )
        fa.deactivateNodes(
            [self.NEW_NODE_NAME], self.tmp_files[1], self.tmp_files[2],
            test_mode=True
        )

        nc = NodeConfig()
        root = nc.get_current_config_root(self.tmp_files[2])
        pm_count = int(root.find('./PrimitiveServers/Count').text)
        self.assertEqual(pm_count, 1)
        node = root.find('./PMS1/IPAddr')
        self.assertEqual(node.text, hostaddr)
        # TODO: Fix node_manipulation add_node logic and _replace_localhost
        # node = root.find('./PMS2/IPAddr')
        # self.assertEqual(node, None)
        node = root.find('./pm1_WriteEngineServer/IPAddr')
        self.assertTrue(node.text, hostaddr)
        node = root.find('./pm2_WriteEngineServer/IPAddr')
        self.assertTrue(node is None)
        #node = root.find("./ConfigRevision")
        #self.assertEqual(node.text, "3")

        # make sure there are no traces of mysql.com,
        # or an ip addr that isn't localhost or 127.0.0.1
        all_nodes = root.findall('./')
        for node in all_nodes:
            self.assertFalse(node.text == self.NEW_NODE_NAME)
            if node.tag in ['IPAddr', 'Node']:
                self.assertTrue(node.text in [hostname, hostaddr])

    def test_designatePrimaryNode(self):
        self.tmp_files = (
            './primary-node0.xml', './primary-node1.xml', './primary-node2.xml'
        )
        fa = FailoverAgent()
        hostaddr = socket.gethostbyname(socket.gethostname())
        with patch.object(NetworkManager, 'validate_hostname_fwd_rev', return_value=None):
            fa.activateNodes(
                [self.NEW_NODE_NAME], tmp_mcs_config_filename, self.tmp_files[0],
                test_mode=True
            )
        add_node(
            hostaddr, self.tmp_files[0], self.tmp_files[1]
        )
        fa.movePrimaryNode(
            'placeholder', self.tmp_files[1], self.tmp_files[2], test_mode=True
        )

        nc = NodeConfig()
        root = nc.get_current_config_root(self.tmp_files[2])
        pm_count = int(root.find('./PrimitiveServers/Count').text)
        self.assertEqual(pm_count, 2)
        node = root.find('./PMS1/IPAddr')
        self.assertEqual(node.text, self.NEW_NODE_NAME)
        node = root.find('./PMS2/IPAddr')
        self.assertEqual(node.text, hostaddr)
        node = root.find('./pm1_WriteEngineServer/IPAddr')
        self.assertEqual(node.text, self.NEW_NODE_NAME)
        node = root.find('./pm2_WriteEngineServer/IPAddr')
        self.assertEqual(node.text, hostaddr)

        for tag in ['ExeMgr1', 'DMLProc', 'DDLProc']:
            node = root.find(f'./{tag}/IPAddr')
            self.assertEqual(node.text, self.NEW_NODE_NAME)

        self.assertEqual(self.NEW_NODE_NAME, root.find('./PrimaryNode').text)

    def test_enterStandbyMode(self):
        fa = FailoverAgent()
        fa.enterStandbyMode(test_mode=True)
