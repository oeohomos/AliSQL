/*
   Copyright (C) 2003 MySQL AB
    All rights reserved. Use is subject to license terms.

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301  USA
*/

#include <NDBT.hpp>
#include <NDBT_Test.hpp>
#include <HugoTransactions.hpp>
#include <UtilTransactions.hpp>
#include <NdbRestarter.hpp>
#include <AtrtClient.hpp>
#include <Bitmask.hpp>
#include <NdbBackup.hpp>

static Vector<BaseString> table_list;

struct NodeInfo
{
  int nodeId;
  int processId;
  int nodeGroup;
};

/**
  Test that one node at a time can be upgraded
*/

int runUpgrade_NR1(NDBT_Context* ctx, NDBT_Step* step){
  AtrtClient atrt;

  SqlResultSet clusters;
  if (!atrt.getClusters(clusters))
    return NDBT_FAILED;

  while (clusters.next())
  {
    uint clusterId= clusters.columnAsInt("id");
    SqlResultSet tmp_result;
    if (!atrt.getConnectString(clusterId, tmp_result))
      return NDBT_FAILED;

    NdbRestarter restarter(tmp_result.column("connectstring"));
    restarter.setReconnect(true); // Restarting mgmd
    g_err << "Cluster '" << clusters.column("name")
          << "@" << tmp_result.column("connectstring") << "'" << endl;

    if (restarter.waitClusterStarted())
      return NDBT_FAILED;

    // Restart ndb_mgmd(s)
    SqlResultSet mgmds;
    if (!atrt.getMgmds(clusterId, mgmds))
      return NDBT_FAILED;

    while (mgmds.next())
    {
      ndbout << "Restart mgmd " << mgmds.columnAsInt("node_id") << endl;
      if (!atrt.changeVersion(mgmds.columnAsInt("id"), ""))
        return NDBT_FAILED;

      if (restarter.waitConnected())
        return NDBT_FAILED;
      ndbout << "Connected to mgmd"<< endl;
    }

    ndbout << "Waiting for started"<< endl;
    if (restarter.waitClusterStarted())
      return NDBT_FAILED;
    ndbout << "Started"<< endl;

    // Restart ndbd(s)
    SqlResultSet ndbds;
    if (!atrt.getNdbds(clusterId, ndbds))
      return NDBT_FAILED;

    while(ndbds.next())
    {
      int nodeId = ndbds.columnAsInt("node_id");
      int processId = ndbds.columnAsInt("id");
      ndbout << "Restart node " << nodeId << endl;

      if (!atrt.changeVersion(processId, ""))
        return NDBT_FAILED;

      if (restarter.waitNodesNoStart(&nodeId, 1))
        return NDBT_FAILED;

      if (restarter.startNodes(&nodeId, 1))
        return NDBT_FAILED;

      if (restarter.waitNodesStarted(&nodeId, 1))
        return NDBT_FAILED;

    }
  }

  ctx->stopTest();
  return NDBT_OK;
}

static
int
runUpgrade_Half(NDBT_Context* ctx, NDBT_Step* step)
{
  // Assuming 2 replicas

  AtrtClient atrt;

  const bool waitNode = ctx->getProperty("WaitNode", Uint32(0)) != 0;
  const char * args = "";
  if (ctx->getProperty("KeepFS", Uint32(0)) != 0)
  {
    args = "--initial=0";
  }

  SqlResultSet clusters;
  if (!atrt.getClusters(clusters))
    return NDBT_FAILED;

  while (clusters.next())
  {
    uint clusterId= clusters.columnAsInt("id");
    SqlResultSet tmp_result;
    if (!atrt.getConnectString(clusterId, tmp_result))
      return NDBT_FAILED;

    NdbRestarter restarter(tmp_result.column("connectstring"));
    restarter.setReconnect(true); // Restarting mgmd
    g_err << "Cluster '" << clusters.column("name")
          << "@" << tmp_result.column("connectstring") << "'" << endl;

    if(restarter.waitClusterStarted())
      return NDBT_FAILED;

    // Restart ndb_mgmd(s)
    SqlResultSet mgmds;
    if (!atrt.getMgmds(clusterId, mgmds))
      return NDBT_FAILED;

    while (mgmds.next())
    {
      ndbout << "Restart mgmd" << mgmds.columnAsInt("node_id") << endl;
      if (!atrt.changeVersion(mgmds.columnAsInt("id"), ""))
        return NDBT_FAILED;

      if(restarter.waitConnected())
        return NDBT_FAILED;
    }

    NdbSleep_SecSleep(5); // TODO, handle arbitration

    // Restart one ndbd in each node group
    SqlResultSet ndbds;
    if (!atrt.getNdbds(clusterId, ndbds))
      return NDBT_FAILED;

    Vector<NodeInfo> nodes;
    while (ndbds.next())
    {
      struct NodeInfo n;
      n.nodeId = ndbds.columnAsInt("node_id");
      n.processId = ndbds.columnAsInt("id");
      n.nodeGroup = restarter.getNodeGroup(n.nodeId);
      nodes.push_back(n);
    }

    int nodesarray[256];
    int cnt= 0;

    Bitmask<4> seen_groups;
    Bitmask<4> restarted_nodes;
    for (Uint32 i = 0; i<nodes.size(); i++)
    {
      int nodeId = nodes[i].nodeId;
      int processId = nodes[i].processId;
      int nodeGroup= nodes[i].nodeGroup;

      if (seen_groups.get(nodeGroup))
      {
        // One node in this node group already down
        continue;
      }
      seen_groups.set(nodeGroup);
      restarted_nodes.set(nodeId);

      ndbout << "Restart node " << nodeId << endl;
      
      if (!atrt.changeVersion(processId, args))
        return NDBT_FAILED;
      
      if (waitNode)
      {
        restarter.waitNodesNoStart(&nodeId, 1);
      }

      nodesarray[cnt++]= nodeId;
    }
    
    if (!waitNode)
    {
      if (restarter.waitNodesNoStart(nodesarray, cnt))
        return NDBT_FAILED;
    }

    ndbout << "Starting and wait for started..." << endl;
    if (restarter.startAll())
      return NDBT_FAILED;

    if (restarter.waitClusterStarted())
      return NDBT_FAILED;

    // Restart the remaining nodes
    cnt= 0;
    for (Uint32 i = 0; i<nodes.size(); i++)
    {
      int nodeId = nodes[i].nodeId;
      int processId = nodes[i].processId;

      if (restarted_nodes.get(nodeId))
        continue;
      
      ndbout << "Restart node " << nodeId << endl;
      if (!atrt.changeVersion(processId, args))
        return NDBT_FAILED;

      if (waitNode)
      {
        restarter.waitNodesNoStart(&nodeId, 1);
      }

      nodesarray[cnt++]= nodeId;
    }

    
    if (!waitNode)
    {
      if (restarter.waitNodesNoStart(nodesarray, cnt))
        return NDBT_FAILED;
    }

    ndbout << "Starting and wait for started..." << endl;
    if (restarter.startAll())
      return NDBT_FAILED;
    
    if (restarter.waitClusterStarted())
      return NDBT_FAILED;
  }

  return NDBT_OK;
}



/**
   Test that one node in each nodegroup can be upgraded simultaneously
    - using method1
*/

int runUpgrade_NR2(NDBT_Context* ctx, NDBT_Step* step)
{
  // Assuming 2 replicas

  ctx->setProperty("WaitNode", 1);
  int res = runUpgrade_Half(ctx, step);
  ctx->stopTest();
  return res;
}

/**
   Test that one node in each nodegroup can be upgrade simultaneously
    - using method2, ie. don't wait for "nostart" before stopping
      next node
*/

int runUpgrade_NR3(NDBT_Context* ctx, NDBT_Step* step){
  // Assuming 2 replicas

  int res = runUpgrade_Half(ctx, step);
  ctx->stopTest();
  return res;
}

int runCheckStarted(NDBT_Context* ctx, NDBT_Step* step){

  // Check cluster is started
  NdbRestarter restarter;
  if(restarter.waitClusterStarted() != 0){
    g_err << "All nodes was not started " << endl;
    return NDBT_FAILED;
  }

  // Check atrtclient is started
  AtrtClient atrt;
  if(!atrt.waitConnected()){
    g_err << "atrt server was not started " << endl;
    return NDBT_FAILED;
  }

  // Make sure atrt assigns nodeid != -1
  SqlResultSet procs;
  if (!atrt.doQuery("SELECT * FROM process", procs))
    return NDBT_FAILED;

  while (procs.next())
  {
    if (procs.columnAsInt("node_id") == (unsigned)-1){
      ndbout << "Found one process with node_id -1, "
             << "use --fix-nodeid=1 to atrt to fix this" << endl;
      return NDBT_FAILED;
    }
  }

  return NDBT_OK;
}

int 
runCreateAllTables(NDBT_Context* ctx, NDBT_Step* step)
{
  ndbout_c("createAllTables");
  if (NDBT_Tables::createAllTables(GETNDB(step), false, true))
    return NDBT_FAILED;

  for (int i = 0; i<NDBT_Tables::getNumTables(); i++)
    table_list.push_back(BaseString(NDBT_Tables::getTable(i)->getName()));

  return NDBT_OK;
}

int
runCreateOneTable(NDBT_Context* ctx, NDBT_Step* step)
{
  // Table is already created...
  // so we just add it to table_list
  table_list.push_back(BaseString(ctx->getTab()->getName()));

  return NDBT_OK;
}

int
runLoadAll(NDBT_Context* ctx, NDBT_Step* step)
{
  Ndb* pNdb = GETNDB(step);
  NdbDictionary::Dictionary * pDict = pNdb->getDictionary();
  int records = ctx->getNumRecords();
  int result = NDBT_OK;
  
  for (unsigned i = 0; i<table_list.size(); i++)
  {
    const NdbDictionary::Table* tab = pDict->getTable(table_list[i].c_str());
    HugoTransactions trans(* tab);
    trans.loadTable(pNdb, records);
    trans.scanUpdateRecords(pNdb, records);
  }
  
  return result;
}


int
runBasic(NDBT_Context* ctx, NDBT_Step* step)
{
  Ndb* pNdb = GETNDB(step);
  NdbDictionary::Dictionary * pDict = pNdb->getDictionary();
  int records = ctx->getNumRecords();
  int result = NDBT_OK;

  int l = 0;
  while (!ctx->isTestStopped())
  {
    l++;
    for (unsigned i = 0; i<table_list.size(); i++)
    {
      const NdbDictionary::Table* tab = pDict->getTable(table_list[i].c_str());
      HugoTransactions trans(* tab);
      switch(l % 3){
      case 0:
        trans.loadTable(pNdb, records);
        trans.scanUpdateRecords(pNdb, records);
        trans.pkUpdateRecords(pNdb, records);
        break;
      case 1:
        trans.scanUpdateRecords(pNdb, records);
        // TODO make pkInterpretedUpdateRecords work on any table
        // (or check if it does)
        if (strcmp(tab->getName(), "T1") == 0)
          trans.pkInterpretedUpdateRecords(pNdb, records);
        trans.clearTable(pNdb, records/2);
        trans.loadTable(pNdb, records/2);
        break;
      case 2:
        trans.clearTable(pNdb, records/2);
        trans.loadTable(pNdb, records/2);
        trans.clearTable(pNdb, records/2);
        break;
      }
    }
  }
  
  return result;
}

int
rollingRestart(NDBT_Context* ctx, NDBT_Step* step)
{
  // Assuming 2 replicas

  AtrtClient atrt;

  SqlResultSet clusters;
  if (!atrt.getClusters(clusters))
    return NDBT_FAILED;

  while (clusters.next())
  {
    uint clusterId= clusters.columnAsInt("id");
    SqlResultSet tmp_result;
    if (!atrt.getConnectString(clusterId, tmp_result))
      return NDBT_FAILED;

    NdbRestarter restarter(tmp_result.column("connectstring"));
    if (restarter.rollingRestart())
      return NDBT_FAILED;
  }
  
  return NDBT_OK;

}

int runUpgrade_Traffic(NDBT_Context* ctx, NDBT_Step* step){
  // Assuming 2 replicas
  
  ndbout_c("upgrading");
  int res = runUpgrade_Half(ctx, step);
  if (res == NDBT_OK)
  {
    ndbout_c("rolling restarting");
    res = rollingRestart(ctx, step);
  }
  ctx->stopTest();
  return res;
}

int
startPostUpgradeChecks(NDBT_Context* ctx, NDBT_Step* step)
{
  /**
   * This will restart *self* in new version
   */

  /**
   * mysql-getopt works so that passing "-n X -n Y" is ok
   *   and is interpreted as "-n Y"
   *
   * so we restart ourselves with testcase-name and "--post-upgrade" appended
   * e.g if testcase is "testUpgrade -n X"
   *     this will restart it as "testUpgrade -n X -n X--post-upgrade"
   */
  BaseString tc;
  tc.assfmt("-n %s--post-upgrade", ctx->getCase()->getName());

  ndbout << "About to restart self with extra arg: " << tc.c_str() << endl;

  AtrtClient atrt;
  int process_id = atrt.getOwnProcessId();
  if (process_id == -1)
  {
    g_err << "Failed to find own process id" << endl;
    return NDBT_FAILED;
  }

  if (!atrt.changeVersion(process_id, tc.c_str()))
    return NDBT_FAILED;

  // Will not be reached...

  return NDBT_OK;
}

int
runPostUpgradeChecks(NDBT_Context* ctx, NDBT_Step* step)
{
  /**
   * Table will be dropped/recreated
   *   automatically by NDBT...
   *   so when we enter here, this is already tested
   */

  NdbBackup backup(GETNDB(step)->getNodeId()+1);

  ndbout << "Starting backup..." << flush;
  if (backup.start() != 0)
  {
    ndbout << "Failed" << endl;
    return NDBT_FAILED;
  }
  ndbout << "done" << endl;

  return NDBT_OK;
}

NDBT_TESTSUITE(testUpgrade);
TESTCASE("Upgrade_NR1",
	 "Test that one node at a time can be upgraded"){
  INITIALIZER(runCheckStarted);
  STEP(runUpgrade_NR1);
  VERIFIER(startPostUpgradeChecks);
}
POSTUPGRADE("Upgrade_NR1")
{
  INITIALIZER(runCheckStarted);
  INITIALIZER(runPostUpgradeChecks);
}
TESTCASE("Upgrade_NR2",
	 "Test that one node in each nodegroup can be upgradde simultaneously"){
  INITIALIZER(runCheckStarted);
  STEP(runUpgrade_NR2);
  VERIFIER(startPostUpgradeChecks);
}
POSTUPGRADE("Upgrade_NR2")
{
  INITIALIZER(runCheckStarted);
  INITIALIZER(runPostUpgradeChecks);
}
TESTCASE("Upgrade_NR3",
	 "Test that one node in each nodegroup can be upgradde simultaneously"){
  INITIALIZER(runCheckStarted);
  STEP(runUpgrade_NR3);
  VERIFIER(startPostUpgradeChecks);
}
POSTUPGRADE("Upgrade_NR3")
{
  INITIALIZER(runCheckStarted);
  INITIALIZER(runPostUpgradeChecks);
}
TESTCASE("Upgrade_FS",
	 "Test that one node in each nodegroup can be upgrade simultaneously")
{
  TC_PROPERTY("KeepFS", 1);
  INITIALIZER(runCheckStarted);
  INITIALIZER(runCreateAllTables);
  INITIALIZER(runLoadAll);
  STEP(runUpgrade_Traffic);
  VERIFIER(startPostUpgradeChecks);
}
POSTUPGRADE("Upgrade_FS")
{
  INITIALIZER(runCheckStarted);
  INITIALIZER(runPostUpgradeChecks);
}
TESTCASE("Upgrade_Traffic",
	 "Test upgrade with traffic, all tables and restart --initial")
{
  INITIALIZER(runCheckStarted);
  INITIALIZER(runCreateAllTables);
  STEP(runUpgrade_Traffic);
  STEP(runBasic);
  VERIFIER(startPostUpgradeChecks);
}
POSTUPGRADE("Upgrade_Traffic")
{
  INITIALIZER(runCheckStarted);
  INITIALIZER(runPostUpgradeChecks);
}
TESTCASE("Upgrade_Traffic_FS",
	 "Test upgrade with traffic, all tables and restart using FS")
{
  TC_PROPERTY("KeepFS", 1);
  INITIALIZER(runCheckStarted);
  INITIALIZER(runCreateAllTables);
  STEP(runUpgrade_Traffic);
  STEP(runBasic);
  VERIFIER(startPostUpgradeChecks);
}
POSTUPGRADE("Upgrade_Traffic_FS")
{
  INITIALIZER(runCheckStarted);
  INITIALIZER(runPostUpgradeChecks);
}
TESTCASE("Upgrade_Traffic_one",
	 "Test upgrade with traffic, *one* table and restart --initial")
{
  INITIALIZER(runCheckStarted);
  INITIALIZER(runCreateOneTable);
  STEP(runUpgrade_Traffic);
  STEP(runBasic);
  VERIFIER(startPostUpgradeChecks);
}
POSTUPGRADE("Upgrade_Traffic_one")
{
  INITIALIZER(runCheckStarted);
  INITIALIZER(runPostUpgradeChecks);
}
TESTCASE("Upgrade_Traffic_FS_one",
	 "Test upgrade with traffic, all tables and restart using FS")
{
  TC_PROPERTY("KeepFS", 1);
  INITIALIZER(runCheckStarted);
  INITIALIZER(runCreateOneTable);
  STEP(runUpgrade_Traffic);
  STEP(runBasic);
  VERIFIER(startPostUpgradeChecks);
}
POSTUPGRADE("Upgrade_Traffic_FS_one")
{
  INITIALIZER(runCheckStarted);
  INITIALIZER(runPostUpgradeChecks);
}
NDBT_TESTSUITE_END(testUpgrade);

int main(int argc, const char** argv){
  ndb_init();
  NDBT_TESTSUITE_INSTANCE(testUpgrade);
  testUpgrade.setCreateAllTables(true);
  return testUpgrade.execute(argc, argv);
}

template class Vector<NodeInfo>;
