package simulation;

public class SchedParams {
	public int rate = 25;
	public int ping_count = 4;
	
	public int latency = 18000;
	
	public int experimentId = 0;
	
	public SchedParams newExperiment(int experimentId) {
		SchedParams sp = new SchedParams();
		
		sp.rate = this.rate;
		sp.ping_count = this.ping_count;
		
		sp.latency = this.latency;
		
		sp.experimentId = experimentId;
		
		return sp;
	}
	
	public String toString() {
		return "SchedParams(l = " + rate + ", n = " + ping_count 
							+ ", L = " + latency + ", e = " + experimentId + ")";
	}
}
