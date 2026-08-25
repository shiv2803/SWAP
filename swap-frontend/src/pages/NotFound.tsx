import { Link } from 'wouter';

export default function NotFound() {
  return (
    <div className="orbit-app is-dark">
      <div style={{ minHeight: '100vh', display: 'grid', placeItems: 'center', padding: 24 }}>
        <div style={{ maxWidth: 420 }}>
          <div className="eyebrow">SWAP / not found</div>
          <h1 style={{ font: "600 46px/0.95 'Fraunces', serif", letterSpacing: '-.065em', margin: '12px 0 10px' }}>
            No signal here.
          </h1>
          <p style={{ color: 'var(--muted)', fontSize: 13, lineHeight: 1.6, margin: '0 0 20px' }}>
            That route isn't part of the console. Head back to the command view to see the live link.
          </p>
          <Link href="/" className="done-button" style={{ display: 'inline-block', textDecoration: 'none' }}>
            Back to command view
          </Link>
        </div>
      </div>
    </div>
  );
}
