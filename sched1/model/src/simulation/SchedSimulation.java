package simulation;

import org.cpntools.accesscpn.model.*;
import org.cpntools.accesscpn.model.declaration.*;
import org.cpntools.accesscpn.model.importer.DOMParser;
import org.cpntools.accesscpn.model.monitors.Monitor;
import org.cpntools.accesscpn.engine.highlevel.HighLevelSimulator;
import org.cpntools.accesscpn.engine.highlevel.PacketPrinter;
import org.cpntools.accesscpn.engine.highlevel.checker.Checker;
import org.cpntools.accesscpn.engine.highlevel.instance.Instance;
import org.cpntools.accesscpn.engine.proxy.ProxyDaemon;
import org.cpntools.accesscpn.engine.proxy.ProxySimulator;
import org.cpntools.accesscpn.engine.DaemonSimulator;
import org.cpntools.accesscpn.engine.Simulator;
import org.eclipse.emf.common.notify.Notifier;

import java.lang.Object;
import java.io.File;
import java.math.BigInteger;
import java.net.URL;
import java.net.InetAddress;
import java.util.Random;

import java.util.concurrent.BlockingQueue;
import java.util.concurrent.LinkedBlockingQueue;

public class SchedSimulation {
	static String modelPath = "/pool/devel/cpn/accesscpn-1/sched1-model.cpn";
	static String outputPath = "/tmp/sched1-model-results";
	
	static long duration = 200000000;
	static long reportRate = 20000000;
	
	/* Number of simulators run in parallel */
	static int numSimulators = 8;
	
	/* Number of experiments conducted per parameter set */
	static int numExperiments = 10;
	
	/* Base TCP port of simulators */
	static int basePort = 5000;
	
	static boolean debug = false;
	
	static class SimulateThread extends Thread {
		HighLevelSimulator hls;
		BlockingQueue<SchedParams> q;
		
		SimulateThread(HighLevelSimulator hls, BlockingQueue<SchedParams> q) {
			this.hls = hls;
			this.q = q;
		}
		
		public void run() {
			while(true) {
				try {
					SchedParams sp = q.take();
					try {
						simulate(sp);
					}
					catch(Exception e) {
						synchronized(System.out) {
							System.err.println("Error simulating " + sp.toString() + " : " + e.toString());
						}
						continue;
					}
				}
				catch(InterruptedException ie) {
					break;
				}
			}
		}
		
		private void simulate(SchedParams sp) throws Exception {
			synchronized(System.out) {
				System.out.println("Parsing petri net for simulation " + sp.toString());
			}
			
			PetriNet pn = DOMParser.parse(new URL("file://" + modelPath));
			
			synchronized(System.out) {
				System.out.println("Starting simulation: " + sp.toString() + "...");
			}
			
			/* Generate output name: */
			String monitorName = "sched" 
								  + "_l" + sp.rate 
								  + "_n" + sp.ping_count 
								  + "_L" + sp.latency
								  + "_e" + sp.experimentId;
			
			for(Monitor monitor: pn.getMonitors()) {
				if(monitor.getName().getText().equals("Requests")) {
					monitor.getName().setText(monitorName);
				}
			}
			
			/* Change params */			
			for(HLDeclaration decl: pn.declaration()) {
				if(decl.getText().startsWith("val rate")) {
					MLDeclaration mlDecl = (MLDeclaration) decl.getStructure();
					mlDecl.setCode("val rate = " + sp.rate + "; " + 
								   "val ping_count = " + sp.ping_count + ";");					
					decl.setStructure(mlDecl);
					
					synchronized(System.out) {
						System.out.println(decl.toString());
					}
				}
				
				/* if(decl.getText().startsWith("val latency")) {
					MLDeclaration mlDecl = (MLDeclaration) decl.getStructure();
					mlDecl.setCode("val latency = " + sp.latency + ";");					
					decl.setStructure(mlDecl);
					
					synchronized(System.out) {
						System.out.println(decl.toString());
					}
				} */
			}
			
			Checker checker = new Checker(pn, null, hls);
			try {
				checker.checkEntireModel(modelPath, outputPath);
			}
			catch(org.cpntools.accesscpn.engine.highlevel.checker.ErrorInitializingSMLInterface 
					smle) {
				/* Ignore ? */
			}
			
			/* Disable simulation report (enabled by Checker) */
			hls.setSimulationOptions(false, false, false, false, false, false, false, 
									"", "", "", Long.toString(reportRate), "", "", false, false);
			
			synchronized(System.out) {
				System.out.println("Running simulation: " + sp.toString() + "...");
			}
			
			long time = 0;
			long reportTime = 0;
			while(time < duration) {
				String reason = hls.execute(0);
				reason = reason.replace('\n', ';');
								
				String strTime = hls.getTime();				
				time = Long.decode(strTime);
				
				if(time > reportTime) {
					synchronized(System.out) {
						System.out.println("Time: [" + strTime +  "] simulation: " + sp.toString() + " reason: " + reason);
					}
					reportTime += reportRate;
				}
			}
		}
	
	}
	
	public static void main(String args[]) {
		System.out.println("Model: " + modelPath);
		System.out.println("Output dir: " + outputPath);
		
		try {
			BlockingQueue<SchedParams> q = new LinkedBlockingQueue<SchedParams>();
			int experimentId = 0;
			
			int latencies[] = {18000}; 
			int nthrs[] = {1, 2, 3, 4, 5, 6, 8};
			
			for(int latency: latencies) {
				for(int rate = 5; rate <= 40; rate += 5) {
					SchedParams sp = new SchedParams();
					sp.rate = rate;
					sp.ping_count = 4;
					sp.latency = latency;
					
					for(int e = 0; e < numExperiments; ++e) {	
						SchedParams esp = sp.newExperiment(++experimentId);
						q.put(esp);
						
						System.out.println("Created experiment " + esp.toString());
					}
				}
				
				
				for(int ping_count: nthrs) {
					SchedParams sp = new SchedParams();
					sp.rate = (30 * 4) / ping_count;
					sp.ping_count = ping_count;					
					sp.latency = latency;
					
					for(int e = 0; e < numExperiments; ++e) {
						SchedParams esp = sp.newExperiment(++experimentId);
						q.put(esp);
						
						System.out.println("Created experiment " + esp.toString());
					}
				}
			}
			
			int basePort = SchedSimulation.basePort + (new Random()).nextInt(5) * numSimulators;
			
			for(int portId = 0; portId < numSimulators; ++portId) {
				int port = basePort + portId;
				
				DaemonSimulator ds = new DaemonSimulator(InetAddress.getLocalHost(), 
														 port, 
														 new File("cpn.ML"));
				Simulator s = new Simulator(ds);
				HighLevelSimulator hls = HighLevelSimulator.getHighLevelSimulator(s);
				
				if(debug) {
					PacketPrinter printer = new PacketPrinter(hls);
					printer.attach();
				
					hls.evaluate("CPN'save_debug_info(\"/tmp/cpndebug-" + portId + ".log\");");
				}
				
				SimulateThread simulateThread = new SchedSimulation.SimulateThread(hls, q);
				simulateThread.start();
			}
		}
		catch(Exception e) {
			e.printStackTrace();
			System.err.println(e.toString());
			System.exit(1);
		}
	}
}

