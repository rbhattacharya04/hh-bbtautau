/*! Skim EventTuple.
This file is part of https://github.com/hh-italian-group/hh-bbtautau. */

#include <thread>
#include <functional>

#include "AnalysisTools/Core/include/RootExt.h"
#include "AnalysisTools/Run/include/program_main.h"
#include "h-tautau/Analysis/include/EventTuple.h"
#include "h-tautau/Analysis/include/SummaryTuple.h"
#include "h-tautau/Analysis/include/AnalysisTypes.h"
#include "AnalysisTools/Run/include/EntryQueue.h"
#include "AnalysisTools/Core/include/ProgressReporter.h"
#include "h-tautau/McCorrections/include/EventWeights.h"

struct Arguments {
    REQ_ARG(std::string, treeName);
    REQ_ARG(std::string, originalFileName);
    REQ_ARG(std::string, outputFileName);
	REQ_ARG(std::string, sample_type);
};

namespace analysis {

class TupleSkimmer {
public:
    using Event = ntuple::Event;
    using EventPtr = std::shared_ptr<Event>;
    using EventTuple = ntuple::EventTuple;
    using EventQueue = run::EntryQueue<EventPtr>;
	
	using ExpressTuple = ntuple::ExpressTuple;
	using ExpressEvent = ntuple::ExpressEvent;
	using ExpressPtr   = std::shared_ptr<ExpressEvent>;

    TupleSkimmer(const Arguments& _args) : args(_args), processQueue(100000), writeQueue(100000), eventWeights(Period::Run2016, DiscriminatorWP::Medium) {}

    void Run()
    {
#if ROOT_VERSION_CODE >= ROOT_VERSION(6,6,0)
        ROOT::EnableThreadSafety();
#endif

		outputFile = root_ext::CreateRootFile(args.outputFileName());
		originalFile = root_ext::OpenRootFile(args.originalFileName());
		
		DisabledBranches_read = { "dphi_mumet", "dphi_metsv", "dR_taumu", "mT1", "mT2", "dphi_bbmet", "dphi_bbsv", "dR_bb","n_jets",
			"btag_weight", "ttbar_weight",  "PU_weight", "shape_denominator_weight", "trigger_accepts", "trigger_matches"};

		DisabledBranches_write = { "lhe_particle_pdg", "lhe_particle_p4", "pfMET_cov", "genJets_partoFlavour", "genJets_hadronFlavour",
			"genJets_p4", "genParticles_p4", "genParticles_pdg", "trigger_accepts", "trigger_matches"};
		
		denominator = GetShapeDenominatorWeight(args.originalFileName());

        std::thread process_thread(std::bind(&TupleSkimmer::ProcessThread, this));
        std::thread writer_thread(std::bind(&TupleSkimmer::WriteThread, this, args.treeName(), args.outputFileName()));

        ReadThread(args.treeName(), args.originalFileName());

        std::cout << "Waiting for process and write threads to finish..." << std::endl;
        process_thread.join();
        writer_thread.join();
		
		SaveSummaryTree();
    }

private:
	Float_t GetShapeDenominatorWeight(const std::string& originalFileName)
	{
		std::cout << "Calculating denominator for shape changing weights..." << std::endl;
		
		std::shared_ptr<ExpressTuple> AllEventTuple(new ExpressTuple("all_events", originalFile.get(), true));
		
		Float_t tot_weight = 1.;
		const Long64_t all_entries = AllEventTuple->GetEntries();
		for(Long64_t current_entry = 0; current_entry < all_entries; ++current_entry)
		{
			AllEventTuple->GetEntry(current_entry);
			ExpressPtr event(new ExpressEvent(AllEventTuple->data()));
			Float_t pu = eventWeights.GetPileUpWeight(*event);
			Float_t mc = event->genEventWeight;
			
			Float_t ttbar = 1.;
			if(args.sample_type() == "ttbar")
			{
				ttbar = std::sqrt(std::exp(0.0615 - 0.0005 * event->gen_top_pt) * std::exp(0.0615 - 0.0005*event->gen_topBar_pt));
			}
			tot_weight = tot_weight + (pu*ttbar*mc);
		}
		return tot_weight;
	}


    void ReadThread(const std::string& treeName, const std::string& originalFileName)
    {
		if(args.sample_type() == "data")
		{
			DisabledBranches_read.erase("trigger_accepts");
			DisabledBranches_read.erase("trigger_matches");
		}
		std::shared_ptr<EventTuple> originalTuple(new EventTuple(treeName, originalFile.get(), true, DisabledBranches_read));

        tools::ProgressReporter reporter(10, std::cout, "Starting skimming...");
        const Long64_t n_entries = originalTuple->GetEntries();
        reporter.SetTotalNumberOfEvents(n_entries);
        for(Long64_t current_entry = 0; current_entry < n_entries; ++current_entry) {
            originalTuple->GetEntry(current_entry);
            reporter.Report(current_entry);
            EventPtr event(new Event(originalTuple->data()));
            processQueue.Push(event);
        }
        processQueue.SetAllDone();
        reporter.Report(n_entries, true);
    }

	void SaveSummaryTree()
	{
		/* --- FIXME --- */
		std::cout << "Copying the summary tree..." << std::endl;
		auto original_tree = root_ext::ReadObject<TTree>(*originalFile, "summary");
		original_tree->SetBranchStatus("*",1);

		auto copied_tree = root_ext::CloneObject<TTree>(*original_tree,"summary");
		root_ext::WriteObject<TTree>(*copied_tree, outputFile.get());
		outputFile->Write();
	}

    void ProcessThread()
    {
        EventPtr event;
        while(processQueue.Pop(event)) {
            if(ProcessEvent(*event))
                writeQueue.Push(event);
        }
        writeQueue.SetAllDone();
    }

    void WriteThread(const std::string& treeName, const std::string& outputFileName)
    {
		if(args.sample_type() == "data")
		{
			DisabledBranches_write.erase("trigger_accepts");
			DisabledBranches_write.erase("trigger_matches");
		}
		std::shared_ptr<EventTuple> outputTuple(new EventTuple(treeName, outputFile.get(), false, DisabledBranches_write));

        EventPtr event;
        while(writeQueue.Pop(event)) {
            (*outputTuple)() = *event;
            outputTuple->Fill();
        }

        outputTuple->Write();
    }

	bool ProcessEvent(Event& event)
    {
	
		if (event.jets_p4.size() < 2) return false;
	
        //const EventEnergyScale es = static_cast<EventEnergyScale>(event.eventEnergyScale);
        //if(es != EventEnergyScale::Central) return false;

        static const std::set<std::string> tauID_Names = {
            "againstMuonTight3", "againstElectronVLooseMVA6", "againstElectronTightMVA6", "againstMuonLoose3",
            "byTightIsolationMVArun2v1DBoldDMwLT", "byVTightIsolationMVArun2v1DBoldDMwLT"
        };
		
        /*decltype(event.tauIDs_1) tauIDs_1, tauIDs_2;
        for(const auto& name : tauID_Names) {
            if(event.tauIDs_1.count(name))
                tauIDs_1[name] = event.tauIDs_1.at(name);
            if(event.tauIDs_2.count(name))
                tauIDs_2[name] = event.tauIDs_2.at(name);

        }
        event.tauIDs_1 = tauIDs_1;
        event.tauIDs_2 = tauIDs_2;
		*/
	
		// Event Variables
		event.n_jets = event.jets_p4.size();

		// Event Weights Variables
		event.btag_weight = eventWeights.GetBtagWeight(event);
		event.PU_weight = eventWeights.GetPileUpWeight(event);
		event.ttbar_weight = eventWeights.GetTopPtWeight(event);
		event.shape_denominator_weight = denominator;
		
		// BDT Variables
		event.dphi_mumet = std::abs(ROOT::Math::VectorUtil::DeltaPhi(event.p4_1    , event.pfMET_p4));
		event.dphi_metsv = std::abs(ROOT::Math::VectorUtil::DeltaPhi(event.SVfit_p4, event.pfMET_p4));
		event.dR_taumu = std::abs(ROOT::Math::VectorUtil::DeltaR(event.p4_1, event.p4_2));
		event.mT1 = Calculate_MT(event.p4_1, event.pfMET_p4);
		event.mT2 = Calculate_MT(event.p4_2, event.pfMET_p4);
		
		if (event.jets_p4.size() >= 2)
		{
			ROOT::Math::LorentzVector<ROOT::Math::PtEtaPhiE4D<float>> bsum = event.jets_p4[0] + event.jets_p4[1];
			event.dphi_bbmet = std::abs(ROOT::Math::VectorUtil::DeltaPhi(bsum, event.pfMET_p4));
			event.dphi_bbsv = std::abs(ROOT::Math::VectorUtil::DeltaPhi(bsum, event.SVfit_p4));
			event.dR_bb = std::abs(ROOT::Math::VectorUtil::DeltaR(event.jets_p4[0], event.jets_p4[1]));
		}
		else
		{
			event.dphi_bbmet = -1.;
			event.dphi_bbsv = -1.;
			event.dR_bb = -1.;
		}
		
		// Jets Variables Resizing
		event.jets_csv.resize(2);
		event.jets_rawf.resize(2);
		event.jets_mva.resize(2);
		event.jets_p4.resize(2);
		event.jets_partonFlavour.resize(2);
		event.jets_hadronFlavour.resize(2);
		
        return true;
    }

private:
    Arguments args;
    EventQueue processQueue, writeQueue;
	std::shared_ptr<TFile> originalFile;
	std::shared_ptr<TFile> outputFile;
	mc_corrections::EventWeights eventWeights;
	Float_t denominator;
	std::set<std::string> DisabledBranches_read;
	std::set<std::string> DisabledBranches_write;
};

} // namespace analysis

PROGRAM_MAIN(analysis::TupleSkimmer, Arguments)
