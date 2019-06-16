#ifndef EDGE_MINER_H
#define EDGE_MINER_H
#include "miner.h"

typedef std::unordered_set<int> HashIntSet;
typedef std::vector<std::unordered_set<int> > HashIntSets;
typedef HashIntSets DomainSupport;
typedef QuickPattern<EdgeEmbedding, ElementType> QPattern;
typedef CanonicalGraph<EdgeEmbedding, ElementType> CPattern;
typedef std::unordered_map<QPattern, Frequency> QpMapFreq; // mapping quick pattern to its frequency
typedef std::unordered_map<CPattern, Frequency> CgMapFreq; // mapping canonical pattern to its frequency
typedef std::unordered_map<QPattern, DomainSupport> QpMapDomain; // mapping quick pattern to its domain support
typedef std::unordered_map<CPattern, DomainSupport> CgMapDomain; // mapping canonical pattern to its domain support
typedef galois::substrate::PerThreadStorage<QpMapFreq> LocalQpMapFreq;
typedef galois::substrate::PerThreadStorage<CgMapFreq> LocalCgMapFreq;
typedef galois::substrate::PerThreadStorage<QpMapDomain> LocalQpMapDomain;
typedef galois::substrate::PerThreadStorage<CgMapDomain> LocalCgMapDomain;

class EdgeMiner : public Miner {
public:
	EdgeMiner(Graph *g) { graph = g; }
	virtual ~EdgeMiner() {}
	// given an embedding, extend it with one more edge, and if it is not automorphism, insert the new embedding into the task queue
	void extend_edge(unsigned max_size, const EdgeEmbedding &emb, EdgeEmbeddingQueue &queue) {
		unsigned size = emb.size();
		// get the number of distinct vertices in the embedding
		VertexSet vertices_set;
		for (unsigned i = 0; i < size; i ++) vertices_set.insert(emb.get_vertex(i));
		size_t num_vertices = vertices_set.size();
		// for each vertex in the embedding
		for (unsigned i = 0; i < size; ++i) {
			VertexId src = emb.get_vertex(i);
			// make sure each distinct vertex is expanded only once
			if (emb.get_key(i) == 0) {
				// try edge extension
				for(auto e : graph->edges(src)) {
					GNode dst = graph->getEdgeDst(e);
					BYTE existed = 1;
					// check if this is automorphism
					if(!is_edgeInduced_automorphism(num_vertices, max_size, emb, i, src, dst, vertices_set, existed)) {
						auto dst_label = 0, edge_label = 0;
						#ifdef ENABLE_LABEL
						dst_label = graph->getData(dst);
						//edge_label = graph->getEdgeData(e); // TODO: enable this for FSM
						#endif
						ElementType new_element(dst, (BYTE)existed, edge_label, dst_label, (BYTE)i);
						EdgeEmbedding new_emb(emb);
						new_emb.push_back(new_element);
						// insert the new (extended) embedding into the queue
						queue.push_back(new_emb);
					}
				}
			}
		}
	}
	void quick_aggregate(EdgeEmbeddingQueue &queue, QpMapFreq &qp_map) {
		for (auto emb : queue) {
			QPattern qp(emb);
			if (qp_map.find(qp) != qp_map.end()) {
				qp_map[qp] += 1;
				qp.clean();
			} else qp_map[qp] = 1;
		}
	}
	void quick_aggregate(EdgeEmbeddingQueue &queue, QpMapDomain &qp_map) {
		for (auto emb : queue) {
			QPattern qp(emb);
			if (qp_map.find(qp) != qp_map.end()) {
				for (unsigned i = 0; i < emb.size(); i ++)
					qp_map[qp][i].insert(emb.get_vertex(i));
				qp.clean();
			} else {
				qp_map[qp].resize(emb.size());
				for (unsigned i = 0; i < emb.size(); i ++)
					qp_map[qp][i].insert(emb.get_vertex(i));
			}
		}
	}
	// aggregate embeddings into quick patterns
	inline void quick_aggregate_each(EdgeEmbedding& emb, QpMapFreq& qp_map) {
		// turn this embedding into its quick pattern
		QPattern qp(emb);
		// update frequency for this quick pattern
		if (qp_map.find(qp) != qp_map.end()) {
			// if this quick pattern already exists, increase its count
			qp_map[qp] += 1;
			emb.set_qpid(qp.get_id());
			qp.clean();
		// otherwise add this quick pattern into the map, and set the count as one
		} else {
			qp_map[qp] = 1;
			emb.set_qpid(qp.get_id());
		}
	}
	inline void quick_aggregate_each(EdgeEmbedding& emb, QpMapDomain& qp_map) {
		QPattern qp(emb);
		bool qp_existed = false;
		auto it = qp_map.find(qp);
		if (it == qp_map.end()) {
			qp_map[qp].resize(emb.size());
			emb.set_qpid(qp.get_id());
		} else {
			qp_existed = true;
			emb.set_qpid((it->first).get_id());
		}
		for (unsigned i = 0; i < emb.size(); i ++)
			qp_map[qp][i].insert(emb.get_vertex(i));
		if (qp_existed) qp.clean();
	}
	void canonical_aggregate(const QpMapFreq &qp_map, CgMapFreq &cg_map) {
		for (auto it = qp_map.begin(); it != qp_map.end(); ++it) {
			QPattern qp = it->first;
			unsigned freq = it->second;
			CPattern cg(qp);
			qp.clean();
			if (cg_map.find(cg) != cg_map.end()) cg_map[cg] += freq;
			else cg_map[cg] = freq;
			cg.clean();
		}
	}
	// aggregate quick patterns into canonical patterns.
	inline void canonical_aggregate_each(QPattern &qp, const Frequency freq, CgMapFreq &cg_map) {
		// turn the quick pattern into its canonical pattern
		CPattern cg(qp);
		qp.clean();
		// if this pattern already exists, increase its count
		if (cg_map.find(cg) != cg_map.end()) cg_map[cg] += freq;
		// otherwise add this pattern into the map, and set the count as 'freq'
		else cg_map[cg] = freq;
		cg.clean();
	}
	// aggregate quick patterns into canonical patterns. Construct an id_map from quick pattern ID (qp_id) to canonical pattern ID (cg_id)
	void canonical_aggregate_each(const QPattern &qp, const Frequency freq, CgMapFreq &cg_map, UintMap &id_map) {
		// turn the quick pattern into its canonical pattern
		CPattern cg(qp);
		//assert(cg != NULL);
		int qp_id = qp.get_id();
		int cg_id = cg.get_id();
		slock.lock();
		id_map.insert(std::make_pair(qp_id, cg_id));
		slock.unlock();
		//qp.clean();
		// if this pattern already exists, increase its count
		auto it = cg_map.find(cg);
		if (it != cg_map.end()) {
			cg_map[cg] += freq;
		// otherwise add this pattern into the map, and set the count as 'freq'
		} else {
			cg_map[cg] = freq;
		}
		cg.clean();
	}
	void canonical_aggregate_each(QPattern &qp, const DomainSupport &domainSets, CgMapDomain& cg_map, UintMap &id_map) {
		assert(qp.get_size() == domainSets.size());
		unsigned numDomains = qp.get_size();
		CPattern cg(qp);
		int qp_id = qp.get_id();
		int cg_id = cg.get_id();
		slock.lock();
		id_map.insert(std::make_pair(qp_id, cg_id));
		slock.unlock();
		auto it = cg_map.find(cg);
		if (it == cg_map.end()) {
			cg_map[cg].resize(numDomains);
			qp.set_cgid(cg.get_id());
		} else {
			qp.set_cgid((it->first).get_id());
		}
		for (unsigned i = 0; i < numDomains; i ++) {
			unsigned qp_idx = cg.get_quick_pattern_index(i);
			assert(qp_idx >= 0 && qp_idx < numDomains);
			cg_map[cg][i].insert(domainSets[qp_idx].begin(), domainSets[qp_idx].end());
		}
		cg.clean();
	}
	inline void merge_qp_map(unsigned num, LocalQpMapFreq &qp_localmap, QpMapFreq &qp_map) {
		for (unsigned i = 0; i < qp_localmap.size(); i++) {
			QpMapFreq qp_lmap = *qp_localmap.getLocal(i);
			for (auto element : qp_lmap) {
				if (qp_map.find(element.first) != qp_map.end())
					qp_map[element.first] += element.second;
				else
					qp_map[element.first] = element.second;
			}
		}
	}
	inline void merge_qp_map(unsigned num_domains, LocalQpMapDomain &qp_localmap, QpMapDomain &qp_map) {
		for (unsigned i = 0; i < qp_localmap.size(); i++) {
			QpMapDomain qp_lmap = *qp_localmap.getLocal(i);
			for (auto element : qp_lmap) {
				if (qp_map.find(element.first) == qp_map.end())
					qp_map[element.first].resize(num_domains);
				for (unsigned i = 0; i < num_domains; i ++)
					qp_map[element.first][i].insert((element.second)[i].begin(), (element.second)[i].end());
			}
		}
	}
	inline void merge_cg_map(unsigned num, LocalCgMapFreq &cg_localmap, CgMapFreq &cg_map) {
		for (unsigned i = 0; i < cg_localmap.size(); i++) {
			CgMapFreq cg_lmap = *cg_localmap.getLocal(i);
			for (auto element : cg_lmap) {
				if (cg_map.find(element.first) != cg_map.end())
					cg_map[element.first] += element.second;
				else
					cg_map[element.first] = element.second;
			}
		}
	}
	inline void merge_cg_map(unsigned num_domains, LocalCgMapDomain &cg_localmap, CgMapDomain &cg_map) {
		for (unsigned i = 0; i < cg_localmap.size(); i++) {
			CgMapDomain cg_lmap = *cg_localmap.getLocal(i);
			for (auto element : cg_lmap) {
				if (cg_map.find(element.first) == cg_map.end())
					cg_map[element.first].resize(num_domains);
				for (unsigned i = 0; i < num_domains; i ++)
					cg_map[element.first][i].insert((element.second)[i].begin(), (element.second)[i].end());
			}
		}
	}
	// check if the pattern of each embedding in the queue is frequent
	void filter_all(EdgeEmbeddingQueue &in_queue, const CgMapFreq &cg_map, EdgeEmbeddingQueue &out_queue) {
		for (auto emb : in_queue) {
			QPattern qp(emb);
			CPattern cg(qp);
			qp.clean();
			//assert(cg_map.find(cg) != cg_map.end());
			if(cg_map.at(cg) >= threshold) out_queue.push_back(emb);
			cg.clean();
		}
	}
	// filtering for FSM
	// check if the pattern of a given embedding is frequent, if yes, insert it to the queue
	void filter_each(const EdgeEmbedding &emb, const CgMapFreq &cg_map, EdgeEmbeddingQueue &out_queue) {
		// find the quick pattern of this embedding
		QPattern qp(emb);
		// find the pattern (canonical graph) of this embedding
		CPattern cg(qp);
		qp.clean();
		// compare the count of this pattern with the threshold
		// if the pattern is frequent, insert this embedding into the task queue
		if (cg_map.at(cg) >= threshold) out_queue.push_back(emb);
		cg.clean();
	}
	void filter_all(EdgeEmbeddingQueue &in_queue, const CgMapDomain &cg_map, EdgeEmbeddingQueue &out_queue) {
		for (auto emb : in_queue) {
			QPattern qp(emb);
			CPattern cg(qp);
			qp.clean();
			//assert(cg_map.find(cg) != cg_map.end());
			bool is_frequent = true;
			unsigned numOfDomains = (cg_map.at(cg)).size();
			for (unsigned i = 0; i < numOfDomains; i ++) {
				if ((cg_map.at(cg))[i].size() < threshold) {
					is_frequent = false;
					break;
				}
			}
			if (is_frequent) out_queue.push_back(emb);
			cg.clean();
		}
	}
	inline void filter_each(const EdgeEmbedding &emb, const CgMapDomain &cg_map, EdgeEmbeddingQueue &out_queue) {
		QPattern qp(emb);
		CPattern cg(qp);
		qp.clean();
		bool is_frequent = true;
		unsigned numOfDomains = (cg_map.at(cg)).size();
		for (unsigned i = 0; i < numOfDomains; i ++) {
			if ((cg_map.at(cg))[i].size() < threshold) {
				is_frequent = false;
				break;
			}
		}
		if (is_frequent) out_queue.push_back(emb);
		cg.clean();
	}
	inline void filter_all(EdgeEmbeddingQueue &in_queue, const UintMap &id_map, const UintMap &support_map, EdgeEmbeddingQueue &out_queue) {
		for (auto emb : in_queue) {
			unsigned qp_id = emb.get_qpid();
			unsigned cg_id = id_map.at(qp_id);
			if (support_map.at(cg_id) >= threshold) out_queue.push_back(emb);
		}
	}
	inline void filter_each(const EdgeEmbedding &emb, const UintMap &id_map, const UintMap &support_map, EdgeEmbeddingQueue &out_queue) {
		unsigned qp_id = emb.get_qpid();
		unsigned cg_id = id_map.at(qp_id);
		if (support_map.at(cg_id) >= threshold) out_queue.push_back(emb);
	}
	inline void set_threshold(const unsigned minsup) { threshold = minsup; }
	inline void printout_agg(const CgMapFreq cg_map) {
		for (auto it = cg_map.begin(); it != cg_map.end(); ++it)
			std::cout << "{" << it->first << " --> " << it->second << std::endl;
	}
	inline void printout_agg(const CgMapDomain cg_map) {
		std::vector<unsigned> support(cg_map.size());
		int i = 0;
		for (auto it = cg_map.begin(); it != cg_map.end(); ++it) {
			support[i] = get_support(it->second);
			i ++;
		}
		i = 0;
		for (auto it = cg_map.begin(); it != cg_map.end(); ++it) {
			std::cout << "{" << it->first << " --> " << support[i] << std::endl;
			i ++;
		}
	}
	unsigned support_count(const CgMapDomain cg_map, UintMap &support_map) {
		unsigned count = 0;
		for (auto it = cg_map.begin(); it != cg_map.end(); ++it) {
			unsigned support = get_support(it->second);
			support_map.insert(std::make_pair(it->first.get_id(), support));
			if (support >= threshold) count ++;
		}
		return count;
	}
	unsigned support_count(const CgMapFreq cg_map, UintMap &support_map) {
		unsigned count = 0;
		for (auto it = cg_map.begin(); it != cg_map.end(); ++it) {
			unsigned support = it->second;
			support_map.insert(std::make_pair(it->first.get_id(), support));
			if (support >= threshold) count ++;
		}
		return count;
	}

private:
	unsigned threshold;
	galois::substrate::SimpleLock slock;
	bool is_edgeInduced_automorphism(unsigned num_vertices, unsigned max_size, const EdgeEmbedding& emb, BYTE history, VertexId src, VertexId dst, const VertexSet& vertex_set, BYTE& existed) {
		//check with the first element
		if (dst <= emb.get_vertex(0)) return true;
		//check loop edge
		if (dst == emb.get_vertex(emb.get_history(history))) return true;
		if (vertex_set.find(dst) == vertex_set.end()) existed = 0;
		// number of vertices must be smaller than k.
		if (num_vertices + 1 - existed > max_size) return true;
		//check to see if there already exists the vertex added; if so, just allow to add edge which is (smaller id -> bigger id)
		if (existed && src > dst) return true;
		std::pair<VertexId, VertexId> added_edge(src, dst);
		for (unsigned index = history + 1; index < emb.size(); ++index) {
			std::pair<VertexId, VertexId> edge;
			getEdge(emb, index, edge);
			int cmp = compare(added_edge, edge);
			if(cmp <= 0) return true;
		}
		return false;
	}
	inline bool edge_existed(const EdgeEmbedding& emb, BYTE history, VertexId src, VertexId dst) {
		std::pair<VertexId, VertexId> added_edge(src, dst);
		for (unsigned i = 1; i < emb.size(); ++i) {
			if (emb.get_vertex(i) == dst && emb.get_vertex(emb.get_history(i)) == src)
				return true;
		}
		return false;
	}
	inline void getEdge(const EdgeEmbedding & emb, unsigned index, std::pair<VertexId, VertexId>& edge) {
		edge.first = emb.get_vertex(emb.get_history(index));
		edge.second = emb.get_vertex(index);
		assert(edge.first != edge.second);
	}
	inline void swap(std::pair<VertexId, VertexId>& pair) {
		if (pair.first > pair.second) {
			VertexId tmp = pair.first;
			pair.first = pair.second;
			pair.second = tmp;
		}
	}
	inline int compare(std::pair<VertexId, VertexId>& oneEdge, std::pair<VertexId, VertexId>& otherEdge) {
		swap(oneEdge);
		swap(otherEdge);
		if(oneEdge.first == otherEdge.first) return oneEdge.second - otherEdge.second;
		else return oneEdge.first - otherEdge.first;
	}
	// counting the minimal image based support
	unsigned get_support(HashIntSets domainSets) {
		unsigned numDomains = domainSets.size();
		unsigned support = 0xFFFFFFFF;
		// get the minimal domain size
		for (unsigned j = 0; j < numDomains; j ++)
			if (domainSets[j].size() < support)
				support = domainSets[j].size();
		return support;
	}
};

#endif // EDGE_MINER_HPP_
